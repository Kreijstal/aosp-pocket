// SPDX-License-Identifier: GPL-3.0-only
#include "../../api32/external_files_rpc.h"
#include <cassert>
#include <cstdio>
#include <dirent.h>
#include <sys/wait.h>

using namespace pocket::fuse32;

static int connectTo(const char* path) {
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);assert(fd>=0);
    sockaddr_un address{};address.sun_family=AF_UNIX;
    assert(strlen(path)<sizeof(address.sun_path));strcpy(address.sun_path,path);
    assert(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    return fd;
}
static FileReply reply(int fd) {
    FileReply result{};size_t at=0;
    while(at<sizeof(result)) {
        ssize_t n=read(fd,reinterpret_cast<char*>(&result)+at,sizeof(result)-at);
        assert(n>0);at+=n;
    }
    return result;
}
static unsigned descriptors() {
    DIR* directory=opendir("/proc/self/fd");assert(directory);unsigned count=0;
    while(dirent* entry=readdir(directory))if(entry->d_name[0]!='.')count++;
    assert(closedir(directory)==0);return count;
}
static void lifetime(int socket,int proof=-1,unsigned number=1) {
    assert(number>=1 && number<=9);
    int fd=proof<0?open("/proc/self/stat",O_RDONLY|O_CLOEXEC):proof;assert(fd>=0);
    char marker=0,control[CMSG_SPACE(9*sizeof(int))]{};iovec part{&marker,1};msghdr message{};
    message.msg_iov=&part;message.msg_iovlen=1;message.msg_control=control;message.msg_controllen=CMSG_SPACE(number*sizeof(int));
    cmsghdr* c=CMSG_FIRSTHDR(&message);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(number*sizeof(int));
    for(unsigned i=0;i<number;i++)memcpy(reinterpret_cast<char*>(CMSG_DATA(c))+i*sizeof(int),&fd,sizeof(fd));
    assert(sendmsg(socket,&message,MSG_NOSIGNAL)==1);
    if(proof<0)close(fd);
}
int main() {
    unsigned originalDescriptors=descriptors();
    char directory[]="/tmp/pocket-external-rpc-XXXXXX";assert(mkdtemp(directory));
    std::string endpoint=std::string(directory)+"/files";
    int pair[2];assert(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair)==0);
    {
        Channel channel(pair[0],1000);Files files(channel);
        char stat[4096];ssize_t count=readIdentityFile("/proc/self/stat",stat,sizeof(stat));assert(count>0);
        uint64_t start=pocket_process_start(stat,count);assert(start);
        HostFiles invalid(files,0,getuid(),start);assert(invalid.start(endpoint)==-EINVAL);
        HostFiles broker(files,getpid(),getuid(),start);assert(broker.start(endpoint)==0);
        assert(broker.start(endpoint)==-EINVAL);
        struct stat mode{};assert(lstat(endpoint.c_str(),&mode)==0 && (mode.st_mode&0777)==0600);

        // A child has the same UID, but cannot impersonate the approved host
        // PID. It must receive the denial before submitting any request.
        pid_t child=fork();assert(child>=0);
        if(!child) {
            int fd=connectTo(endpoint.c_str());FileReply denied=reply(fd);close(fd);
            _exit(denied.error==-EACCES && !denied.size?0:1);
        }
        int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);

        FileRequest request{ExternalMagic,1,static_cast<uint32_t>(FileOp::Size),3,1,0,0,0,0,0};
        int fd=connectTo(endpoint.c_str());lifetime(fd);assert(write(fd,&request,sizeof(request))==sizeof(request));
        assert(reply(fd).error==-EBADF);close(fd);
        request.reserved=1;
        fd=connectTo(endpoint.c_str());lifetime(fd);assert(write(fd,&request,sizeof(request))==sizeof(request));
        assert(reply(fd).error==-EINVAL);close(fd);
        request.reserved=0;request.size=131073;
        fd=connectTo(endpoint.c_str());lifetime(fd);assert(write(fd,&request,sizeof(request))==sizeof(request));
        assert(reply(fd).error==-EINVAL);close(fd);

        fd=connectTo(endpoint.c_str());char marker=0;assert(write(fd,&marker,1)==1);
        assert(reply(fd).error==-EACCES);close(fd);
        // Copied stat text in a regular file cannot substitute for the
        // parent's kernel lifetime descriptor.
        std::string fake=std::string(directory)+"/fake-stat";
        int copied=open(fake.c_str(),O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);assert(copied>=0);
        assert(write(copied,stat,count)==count);
        fd=connectTo(endpoint.c_str());lifetime(fd,copied);assert(reply(fd).error==-EACCES);close(fd);close(copied);assert(unlink(fake.c_str())==0);
        for(unsigned number:{2U,9U}) {
            fd=connectTo(endpoint.c_str());lifetime(fd,-1,number);assert(reply(fd).error==-EACCES);close(fd);
        }

        // A connected client that never supplies a complete frame must not
        // hold shutdown behind the socket's normal 15-second deadline.
        fd=connectTo(endpoint.c_str());lifetime(fd);assert(write(fd,&request,1)==1);
        auto began=std::chrono::steady_clock::now();broker.stop();close(fd);
        assert(std::chrono::steady_clock::now()-began<std::chrono::seconds(2));
        assert(access(endpoint.c_str(),F_OK)==-1 && errno==ENOENT);
        assert(broker.denied==5 && broker.reads==0 && broker.writes==0);
        HostFiles stale(files,getpid(),getuid(),start+1);assert(stale.start(endpoint)==0);
        fd=connectTo(endpoint.c_str());lifetime(fd);assert(reply(fd).error==-EACCES);close(fd);stale.stop();
    }
    close(pair[1]);assert(rmdir(directory)==0);
    assert(descriptors()==originalDescriptors);
    puts("EXTERNAL_FILES_RPC_PASS peer_pid_denial=1 kernel_lifetime=1 missing_lifetime_denied=1 copied_stat_denied=1 stale_start_denied=1 excess_rights_denied=1 truncated_rights_denied=1 invalid_capability=1 malformed_frames=2 partial_frame_shutdown=1 endpoint_cleanup=1 fd_leaks=0");
}
