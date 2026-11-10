// SPDX-License-Identifier: GPL-3.0-only
#include "../../api32/external_open_rpc.h"
#include <cassert>
#include <dirent.h>
using namespace pocket::fuse32;
static unsigned descriptors() {
    DIR* dir=opendir("/proc/self/fd");assert(dir);unsigned count=0;
    while(dirent* entry=readdir(dir))if(entry->d_name[0]!='.')count++;
    closedir(dir);return count;
}
static int connectTo(const std::string& path) {
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);assert(fd>=0);
    sockaddr_un address{};address.sun_family=AF_UNIX;assert(path.size()<sizeof(address.sun_path));strcpy(address.sun_path,path.c_str());
    assert(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);return fd;
}
static int request(const std::string& path,const PocketExternalOpen& request) {
    int fd=connectTo(path);assert(send(fd,&request,sizeof(request),MSG_NOSIGNAL)==sizeof(request));
    PocketExternalReply reply{};size_t at=0;
    while(at<sizeof(reply)){ssize_t n=read(fd,reinterpret_cast<char*>(&reply)+at,sizeof(reply)-at);assert(n>0);at+=n;}
    close(fd);assert(!reply.reserved);return reply.error;
}
int main() {
    unsigned initial=descriptors();char directory[]="/tmp/pocket-external-open-XXXXXX";assert(mkdtemp(directory));
    std::string socket=std::string(directory)+"/app",locator=std::string(directory)+"/locator",host=std::string(directory)+"/host";
    int pair[2];assert(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair)==0);
    {
        Channel channel(pair[0],1000);Files files(channel);
        char stat[4096];ssize_t size=readIdentityFile("/proc/self/stat",stat,sizeof(stat));assert(size>0);uint64_t start=pocket_process_start(stat,size);assert(start);
        AppFiles app(files,getpid(),getuid()+1,start);assert(app.start(socket,host,locator)==0);
        PocketExternalLocator info{};assert(readIdentityFile(locator.c_str(),&info,sizeof(info))==sizeof(info));
        assert(info.magic==POCKET_EXTERNAL_LOCATOR_MAGIC && info.version==1 && info.pid==static_cast<unsigned>(getpid()) && info.start==start && !info.reserved);
        AppFiles collision(files,getpid(),getuid(),start);assert(collision.start(socket,host,locator)==-EADDRINUSE);
        assert(access(socket.c_str(),F_OK)==0 && access(locator.c_str(),F_OK)==0);
        PocketExternalOpen open{POCKET_EXTERNAL_OPEN_MAGIC,1,O_RDONLY,static_cast<uint32_t>(getpid()),1,0,{0,0}};
        assert(request(socket,open)==-EACCES); // kernel UID cannot be supplied in a request
        for(uint32_t flag:{O_CREAT,O_TRUNC,O_DIRECTORY}) {
            open.flags=flag;assert(request(socket,open)==-EINVAL);
        }
        open.flags=O_RDONLY;
        open.magic=0;assert(request(socket,open)==-EINVAL);open.magic=POCKET_EXTERNAL_OPEN_MAGIC;
        open.path_size=4096;assert(request(socket,open)==-EINVAL);open.path_size=1;
        open.tid=0;assert(request(socket,open)==-EINVAL);
        int pending=connectTo(socket);assert(write(pending,&open,1)==1);
        auto began=std::chrono::steady_clock::now();app.stop();assert(std::chrono::steady_clock::now()-began<std::chrono::seconds(2));close(pending);
        assert(app.opened==0 && app.denied==7 && files.count()==0);
        assert(access(socket.c_str(),F_OK)==-1 && errno==ENOENT);assert(access(locator.c_str(),F_OK)==-1 && errno==ENOENT);
        // Startup failure must not erase somebody else's locator.
        int fd=openat(AT_FDCWD,locator.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);assert(fd>=0 && write(fd,"keep",4)==4);close(fd);
        AppFiles stale(files,getpid(),getuid(),start);assert(stale.start(socket,host,locator)==-EEXIST);
        char kept[4];assert(readIdentityFile(locator.c_str(),kept,sizeof(kept))==4 && !memcmp(kept,"keep",4));assert(unlink(locator.c_str())==0);
        assert(access(socket.c_str(),F_OK)==-1 && errno==ENOENT);
    }
    close(pair[1]);assert(rmdir(directory)==0 && descriptors()==initial);
    puts("EXTERNAL_OPEN_RPC_PASS locator_lifetime=1 bind_collision=1 wrong_kernel_uid_denied=1 malformed_frames=3 v1_mutation_and_directory_flags_denied=1 pending_shutdown=1 stale_locator_preserved=1 fd_leaks=0");
}
