// SPDX-License-Identifier: GPL-3.0-only
// Real host AppFuse descriptors, consumed by the guest Bionic through Pocket.
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"FUSE_PROXY32_FAIL line=%d errno=%d\n",__LINE__,errno);std::exit(1);}}while(0)
int main(int argc,char** argv) {
    REQUIRE(argc==2 && strlen(argv[1])==32);
    REQUIRE(setgid(19992)==0 && setuid(19991)==0);
    int control=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);REQUIRE(control>=0);
    sockaddr_un address{};address.sun_family=AF_UNIX;
    strcpy(address.sun_path,"/dev/socket/external-test");
    REQUIRE(connect(control,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    REQUIRE(write(control,argv[1],32)==32);
    int files[4];char tag=0;char ancillary[CMSG_SPACE(sizeof(files))]{};
    iovec vector{&tag,1};msghdr message{};message.msg_iov=&vector;message.msg_iovlen=1;
    message.msg_control=ancillary;message.msg_controllen=sizeof(ancillary);
    REQUIRE(recvmsg(control,&message,MSG_CMSG_CLOEXEC)==1 && tag=='F' && !(message.msg_flags&MSG_CTRUNC));
    cmsghdr* rights=CMSG_FIRSTHDR(&message);
    REQUIRE(rights && rights->cmsg_level==SOL_SOCKET && rights->cmsg_type==SCM_RIGHTS && rights->cmsg_len==CMSG_LEN(sizeof(files)));
    memcpy(files,CMSG_DATA(rights),sizeof(files));
    int mainTid=static_cast<int>(syscall(__NR_gettid));int workerTid=0;
    printf("MAIN_TID=%d\n",mainTid);
    char bytes[32]{};const char text[]="external-proxy-canary";
    REQUIRE(pwrite(files[0],text,sizeof(text),0)==sizeof(text));
    int duplicate=dup(files[0]);REQUIRE(duplicate>=0);
    REQUIRE(pread(duplicate,bytes,sizeof(text),0)==sizeof(text) && !memcmp(bytes,text,sizeof(text)));
    auto mapping=static_cast<char*>(mmap(nullptr,8192,PROT_READ|PROT_WRITE,MAP_SHARED,files[0],0));
    REQUIRE(mapping!=MAP_FAILED && !memcmp(mapping,text,sizeof(text)));
    memcpy(mapping+128,"shared-map",11);REQUIRE(msync(mapping,8192,MS_SYNC)==0);
    memset(bytes,0,sizeof(bytes));REQUIRE(pread(duplicate,bytes,11,128)==11 && !memcmp(bytes,"shared-map",11));
    REQUIRE(munmap(mapping,8192)==0 && fdatasync(files[0])==0 && fsync(files[0])==0);
    errno=0;REQUIRE(pread(files[1],bytes,1,0)==-1 && errno==EACCES);
    std::thread worker([&]{workerTid=static_cast<int>(syscall(__NR_gettid));
        char value=0;REQUIRE(pread(files[2],&value,1,4096)==1 && value=='Q');});
    worker.join();printf("WORKER_TID=%d\n",workerTid);REQUIRE(workerTid!=mainTid);
    std::thread syncing([&]{REQUIRE(fsync(files[2])==0);});
    REQUIRE(read(control,&tag,1)==1 && tag=='S');
    REQUIRE(pread(files[3],bytes,1,0)==1 && bytes[0]=='Q');
    REQUIRE(write(control,"R",1)==1);syncing.join();
    REQUIRE(close(duplicate)==0);
    for(int fd:files)REQUIRE(close(fd)==0);
    REQUIRE(write(control,"D",1)==1 && close(control)==0);
    puts("FUSE_PROXY32_PASS shared_mmap=coherent dup=coherent errno=EACCES blocked_sync_read=PASS uid=19991");
    return 0;
}
