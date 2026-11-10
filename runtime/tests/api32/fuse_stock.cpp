// SPDX-License-Identifier: GPL-3.0-only
#define FUSE_USE_VERSION 32
#include <fuse_lowlevel.h>
#include <fuse_kernel.h>
#include "../../api32/fuse_transport.h"
#include "../../api32/fuse_peer.h"
#include "../../loader/guest_api.h"
#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <thread>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <sys/un.h>

#define REQUIRE(x) do{if(!(x)){std::cerr<<"FUSE_STOCK_FAIL line="<<__LINE__<<" errno="<<errno<<"\n";std::exit(1);}}while(0)
using namespace pocket::fuse32;
static_assert(sizeof(fuse_in_header)==sizeof(RequestHeader));
static_assert(sizeof(fuse_out_header)==sizeof(ResponseHeader));
static_assert(offsetof(fuse_in_header,uid)==offsetof(RequestHeader,uid));
static_assert(offsetof(fuse_in_header,nodeid)==offsetof(RequestHeader,node));
static_assert(offsetof(fuse_out_header,unique)==offsetof(ResponseHeader,unique));
#define API(name) static decltype(&name) p_##name
API(fuse_session_new);API(fuse_session_loop_mt);API(fuse_session_exit);API(fuse_session_destroy);API(fuse_session_fd);
API(fuse_reply_err);API(fuse_reply_entry);API(fuse_reply_open);API(fuse_reply_buf);API(fuse_reply_write);
API(fuse_reply_attr);API(fuse_reply_none);API(fuse_req_ctx);API(fuse_opt_free_args);
static std::atomic<int> initialized{0},destroyed{0},forgotten{0},identities{0};
static std::mutex dataMutex;
static std::mutex syncMutex;
static std::condition_variable syncCondition;
static bool syncEntered=false,syncUnblocked=false;
static std::string data="stock-fuse32-canary";
static bool caller(fuse_req_t req) {
    auto ctx=p_fuse_req_ctx(req);return ctx->uid==19991 && ctx->gid==19992 && ctx->pid==19993;
}
static struct stat attributes() {
    struct stat value{};value.st_ino=2;value.st_mode=S_IFREG|0600;value.st_nlink=1;value.st_uid=19991;value.st_gid=19992;
    std::lock_guard<std::mutex> guard(dataMutex);value.st_size=data.size();return value;
}
static void init(void*,fuse_conn_info* conn) {REQUIRE(conn->proto_major==7 && conn->proto_minor==31);conn->want=0;conn->max_write=8192;initialized++;}
static void destroy(void*){destroyed++;}
static void lookup(fuse_req_t req,fuse_ino_t parent,const char* name) {
    if(!caller(req)){p_fuse_reply_err(req,EACCES);return;}identities++;
    if(parent!=1 || std::strcmp(name,"file")){p_fuse_reply_err(req,ENOENT);return;}
    fuse_entry_param entry{};entry.ino=2;entry.generation=1;entry.attr=attributes();p_fuse_reply_entry(req,&entry);
}
static void getattr(fuse_req_t req,fuse_ino_t node,fuse_file_info*) {
    if(node!=2){p_fuse_reply_err(req,ENOENT);return;}auto st=attributes();p_fuse_reply_attr(req,&st,0);
}
static void openFile(fuse_req_t req,fuse_ino_t node,fuse_file_info* file) {
    if(!caller(req) || node!=2){p_fuse_reply_err(req,EACCES);return;}file->fh=0x1234;p_fuse_reply_open(req,file);
}
static void readFile(fuse_req_t req,fuse_ino_t node,size_t size,off_t offset,fuse_file_info* file) {
    if(!caller(req) || node!=2 || file->fh!=0x1234){p_fuse_reply_err(req,EACCES);return;}
    std::lock_guard<std::mutex> guard(dataMutex);if(offset<0 || static_cast<size_t>(offset)>data.size()){p_fuse_reply_err(req,EINVAL);return;}
    p_fuse_reply_buf(req,data.data()+offset,std::min(size,data.size()-offset));
}
static void writeFile(fuse_req_t req,fuse_ino_t node,const char* bytes,size_t size,off_t offset,fuse_file_info* file) {
    if(!caller(req) || node!=2 || file->fh!=0x1234){p_fuse_reply_err(req,EACCES);return;}
    if(offset!=0 || size>4096){p_fuse_reply_err(req,EINVAL);return;}
    {std::lock_guard<std::mutex> guard(dataMutex);data.assign(bytes,size);}p_fuse_reply_write(req,size);
}
static void releaseFile(fuse_req_t req,fuse_ino_t,fuse_file_info*){p_fuse_reply_err(req,0);}
static void syncFile(fuse_req_t req,fuse_ino_t node,int datasync,fuse_file_info* file) {
    REQUIRE(caller(req) && node==2 && file->fh==0x1234 && !datasync);
    std::unique_lock<std::mutex> lock(syncMutex);syncEntered=true;syncCondition.notify_all();
    REQUIRE(syncCondition.wait_for(lock,std::chrono::seconds(5),[]{return syncUnblocked;}));
    lock.unlock();p_fuse_reply_err(req,0);
}
static void forget(fuse_req_t req,fuse_ino_t node,uint64_t count){REQUIRE(node==2 && count==1);forgotten++;p_fuse_reply_none(req);}
static size_t descriptors() {DIR* dir=opendir("/proc/self/fd");REQUIRE(dir);size_t count=0;while(readdir(dir))count++;closedir(dir);return count;}
static void unixDatagramCheck() {
    int server=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0),client=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);REQUIRE(server>=0 && client>=0);
    sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/uds32");
    REQUIRE(bind(server,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    char received=0;
    REQUIRE(sendto(client,"A",1,0,reinterpret_cast<sockaddr*>(&address),sizeof(address))==1);
    REQUIRE(recv(server,&received,1,0)==1 && received=='A');
    char sent='B';iovec vector{&sent,1};msghdr message{};message.msg_name=&address;message.msg_namelen=sizeof(address);message.msg_iov=&vector;message.msg_iovlen=1;
    REQUIRE(sendmsg(client,&message,0)==1);REQUIRE(recv(server,&received,1,0)==1 && received=='B');
    REQUIRE(connect(client,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    message.msg_name=nullptr;message.msg_namelen=0;sent='C';
    REQUIRE(sendmsg(client,&message,0)==1 && recv(server,&received,1,0)==1 && received=='C');
    int pipe[2];REQUIRE(pipe2(pipe,O_CLOEXEC)==0 && write(pipe[1],"Z",1)==1);
    char outgoing[CMSG_SPACE(sizeof(int))]{};message.msg_name=&address;message.msg_namelen=sizeof(address);message.msg_control=outgoing;message.msg_controllen=sizeof(outgoing);
    cmsghdr* rights=CMSG_FIRSTHDR(&message);rights->cmsg_level=SOL_SOCKET;rights->cmsg_type=SCM_RIGHTS;rights->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(rights),&pipe[0],sizeof(int));sent='D';
    REQUIRE(sendmsg(client,&message,0)==1);
    char incoming[CMSG_SPACE(sizeof(int))]{};iovec readVector{&received,1};msghdr response{};response.msg_iov=&readVector;response.msg_iovlen=1;response.msg_control=incoming;response.msg_controllen=sizeof(incoming);
    REQUIRE(recvmsg(server,&response,MSG_CMSG_CLOEXEC)==1 && received=='D' && !(response.msg_flags&MSG_CTRUNC));
    rights=CMSG_FIRSTHDR(&response);REQUIRE(rights && rights->cmsg_level==SOL_SOCKET && rights->cmsg_type==SCM_RIGHTS && rights->cmsg_len==CMSG_LEN(sizeof(int)));
    int transferred;memcpy(&transferred,CMSG_DATA(rights),sizeof(int));REQUIRE((fcntl(transferred,F_GETFD)&FD_CLOEXEC)!=0);
    REQUIRE(read(transferred,&received,1)==1 && received=='Z');close(transferred);close(pipe[0]);close(pipe[1]);
    close(client);close(server);REQUIRE(unlink(address.sun_path)==0);
}
static void peerCheck(uid_t uid,gid_t gid) {
    uid_t host;REQUIRE(kernelUid(&host)==0);int pair[2];REQUIRE(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair)==0);
    Identity identity{};pid_t tid=static_cast<pid_t>(syscall(__NR_gettid));REQUIRE(peerIdentity(pair[0],tid,host,&identity)==0);
    REQUIRE(identity.uid==uid && identity.gid==gid && identity.pid==static_cast<uint32_t>(tid));
    REQUIRE(peerIdentity(pair[0],tid,host+1,&identity)==-EACCES);
    REQUIRE(peerIdentity(pair[0],0,host,&identity)==-EACCES);
    REQUIRE(peerIdentity(pair[0],getppid(),host,&identity)==-EACCES);
    std::thread worker([&]{pid_t thread=static_cast<pid_t>(syscall(__NR_gettid));Identity current{};
        REQUIRE(thread!=getpid() && peerIdentity(pair[0],thread,host,&current)==0);
        REQUIRE(current.pid==static_cast<uint32_t>(thread) && current.uid==uid && current.gid==gid);});worker.join();
    close(pair[0]);close(pair[1]);
}
int main(int argc,char** argv) {
    if(argc==2 && !std::strcmp(argv[1],"--storage-mode-child")) {
        REQUIRE(getuid()==19991 && getgid()==19992);
        REQUIRE(syscall(__NR_getpid,0x504c2001UL,7UL)==3);
        REQUIRE(syscall(__NR_getpid,0x504c2001UL,4UL)==1);
        gid_t groups[64];REQUIRE(getgroups(64,groups)==64);
        for(unsigned i=0;i<64;i++)REQUIRE(groups[i]==19000+i);
        REQUIRE(!getenv("POCKET_LOADER_STATE_V1"));
        int lower=open("/storage/emulated/0/lower-view",O_RDONLY|O_CLOEXEC);REQUIRE(lower>=0);
        char lowerBytes[32]{};REQUIRE(read(lower,lowerBytes,sizeof(lowerBytes))==sizeof("pocket-lower-view32")-1 && !strcmp(lowerBytes,"pocket-lower-view32"));close(lower);
        struct stat lowerRoot{};REQUIRE(lstat("/storage/emulated",&lowerRoot)==0 && S_ISDIR(lowerRoot.st_mode));
        errno=0;REQUIRE(open("/mnt/user/0/emulated/0/lower-view",O_RDONLY|O_CLOEXEC)==-1 && errno==ENOENT);
        errno=0;REQUIRE(pocket_native_storage_mode(0)==-1 && errno==EPERM);
        peerCheck(19991,19992);
        std::cout<<"FUSE32_STORAGE_MODE_EXEC_PASS mode=3 uid=19991 environment=consumed mutation=denied groups=64 denylist=1 lower_view=verified forced_fuse=not_bypassed\n";return 0;
    }
    REQUIRE(argc==1 && getuid()==0 && syscall(__NR_getpid,0x504c2001UL,7UL)==0);
    unixDatagramCheck();
    peerCheck(0,0);
    REQUIRE(setgid(19992)==0);peerCheck(0,19992);
    REQUIRE(setgid(0)==0);peerCheck(0,0);
    errno=0;REQUIRE(pocket_native_storage_mode(5)==-1 && errno==EINVAL);
    REQUIRE(pocket_native_storage_mode(3)==0);
    gid_t groups[64];for(unsigned i=0;i<64;i++)groups[i]=19000+i;
    REQUIRE(pocket_native_setgroups(64,groups)==0);
    pid_t child=fork();REQUIRE(child>=0);
    if(!child) {
        REQUIRE(syscall(__NR_getpid,0x504c2001UL,7UL)==3);
        REQUIRE(pocket_native_denylist_view()==0);
        REQUIRE(setgid(19992)==0 && setuid(19991)==0);
        errno=0;REQUIRE(pocket_native_storage_mode(0)==-1 && errno==EPERM);
        char executable[]="/system/bin/fuse-stock",mode[]="--storage-mode-child";char* childArgs[]={executable,mode,nullptr};
        execv(executable,childArgs);std::exit(2);
    }
    int status;REQUIRE(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    REQUIRE(pocket_native_storage_mode(0)==0 && syscall(__NR_getpid,0x504c2001UL,7UL)==0);
    REQUIRE(pocket_native_setgroups(0,nullptr)==0);
    size_t before=descriptors();void* lib=dlopen("libfuse.so",RTLD_NOW|RTLD_LOCAL);if(!lib)std::cerr<<dlerror()<<"\n";REQUIRE(lib);
#define LOAD(name) p_##name=reinterpret_cast<decltype(p_##name)>(dlsym(lib,#name));REQUIRE(p_##name)
    LOAD(fuse_session_new);LOAD(fuse_session_loop_mt);LOAD(fuse_session_exit);LOAD(fuse_session_destroy);LOAD(fuse_session_fd);
    LOAD(fuse_reply_err);LOAD(fuse_reply_entry);LOAD(fuse_reply_open);LOAD(fuse_reply_buf);LOAD(fuse_reply_write);
    LOAD(fuse_reply_attr);LOAD(fuse_reply_none);LOAD(fuse_req_ctx);LOAD(fuse_opt_free_args);
    char program[]="pocket-stock-fuse32";char* fuseArgv[]={program};fuse_args args{1,fuseArgv,0};
    fuse_lowlevel_ops ops{};ops.init=init;ops.destroy=destroy;ops.lookup=lookup;ops.getattr=getattr;
    ops.open=openFile;ops.read=readFile;ops.write=writeFile;ops.release=releaseFile;ops.forget=forget;ops.fsync=syncFile;
    auto session=p_fuse_session_new(&args,&ops,sizeof(ops),nullptr);REQUIRE(session);
    int pair[2];REQUIRE(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair)==0);
    // The packaged MediaProvider writes the same prefix's fd member. Verify
    // the ABI with the library's own accessor before starting its loop.
    struct Prefix {char* mountpoint;int exited,fd;};reinterpret_cast<Prefix*>(session)->fd=pair[1];REQUIRE(p_fuse_session_fd(session)==pair[1]);
    std::atomic<int> loopResult{-999};fuse_loop_config config{1,2};
    std::thread loop([&]{loopResult=p_fuse_session_loop_mt(session,&config);});
    {
        Channel channel(pair[0],10000);Identity identity{19991,19992,19993};std::vector<char> out;
        fuse_init_in initialize{7,31,0,0};REQUIRE(channel.call(FUSE_INIT,0,identity,&initialize,sizeof(initialize),&out)==0);
        REQUIRE(out.size()>=sizeof(fuse_init_out));fuse_init_out negotiated{};std::memcpy(&negotiated,out.data(),sizeof(negotiated));
        REQUIRE(negotiated.major==7 && negotiated.minor==31 && negotiated.max_write==8192 && initialized==1);
        const char missing[]="missing";REQUIRE(channel.call(FUSE_LOOKUP,1,identity,missing,sizeof(missing),&out)==-ENOENT);
        const char name[]="file";REQUIRE(channel.call(FUSE_LOOKUP,1,identity,name,sizeof(name),&out)==0 && out.size()==sizeof(fuse_entry_out));
        fuse_entry_out entry{};std::memcpy(&entry,out.data(),sizeof(entry));REQUIRE(entry.nodeid==2 && entry.attr.uid==19991);
        fuse_open_in opening{O_RDWR,0};REQUIRE(channel.call(FUSE_OPEN,2,identity,&opening,sizeof(opening),&out)==0 && out.size()==sizeof(fuse_open_out));
        fuse_open_out opened{};std::memcpy(&opened,out.data(),sizeof(opened));REQUIRE(opened.fh==0x1234);
        fuse_read_in reading{};reading.fh=opened.fh;reading.size=128;
        REQUIRE(channel.call(FUSE_READ,2,{19994,19992,19993},&reading,sizeof(reading),&out)==-EACCES);
        REQUIRE(channel.call(FUSE_READ,2,identity,&reading,sizeof(reading),&out)==0 && std::string(out.begin(),out.end())==data);
        fuse_write_in writing{};writing.fh=opened.fh;writing.size=5;std::vector<char> body(sizeof(writing)+5);
        std::memcpy(body.data(),&writing,sizeof(writing));std::memcpy(body.data()+sizeof(writing),"hello",5);
        REQUIRE(channel.call(FUSE_WRITE,2,identity,body.data(),body.size(),&out)==0 && out.size()==sizeof(fuse_write_out));
        fuse_write_out written{};std::memcpy(&written,out.data(),sizeof(written));REQUIRE(written.size==5);
        REQUIRE(channel.call(FUSE_READ,2,identity,&reading,sizeof(reading),&out)==0 && std::string(out.begin(),out.end())=="hello");
        std::atomic<bool> synced{false};
        std::thread syncing([&]{fuse_fsync_in sync{};sync.fh=opened.fh;std::vector<char> reply;
            REQUIRE(channel.call(FUSE_FSYNC,2,identity,&sync,sizeof(sync),&reply)==0 && reply.empty());synced=true;});
        {std::unique_lock<std::mutex> lock(syncMutex);REQUIRE(syncCondition.wait_for(lock,std::chrono::seconds(3),[]{return syncEntered;}));}
        REQUIRE(channel.call(FUSE_READ,2,identity,&reading,sizeof(reading),&out)==0 && std::string(out.begin(),out.end())=="hello" && !synced);
        {std::lock_guard<std::mutex> lock(syncMutex);syncUnblocked=true;}syncCondition.notify_all();syncing.join();REQUIRE(synced);
        fuse_release_in release{};release.fh=opened.fh;release.flags=O_RDWR;REQUIRE(channel.call(FUSE_RELEASE,2,identity,&release,sizeof(release),&out)==0 && out.empty());
        REQUIRE(channel.forget(2,1,identity)==0);REQUIRE(channel.call(FUSE_DESTROY,0,identity,nullptr,0,&out)==0);
        p_fuse_session_exit(session);channel.cancel();loop.join();REQUIRE(loopResult==0 && destroyed==1 && forgotten==1 && identities>=2);
    }
    p_fuse_session_destroy(session);p_fuse_opt_free_args(&args);REQUIRE(dlclose(lib)==0);REQUIRE(descriptors()==before);
    std::cout<<"FUSE_STOCK32_PASS protocol=7.31 init=1 lookup=1 missing=denied caller_fields=preserved peer_identity=verified thread_id=preserved foreign_thread=denied gid_only_change=verified unix_datagram_paths=verified read=1 wrong_uid=denied write=1 blocked_sync_read=verified out_of_order=verified release=1 forget=1 destroy=1 loop_exit=0 fd_leaks=0 kernel_mounts=0\n";
}
