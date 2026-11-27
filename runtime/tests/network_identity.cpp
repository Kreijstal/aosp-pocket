// SPDX-License-Identifier: GPL-3.0-only
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../api32/network_identity.h"
static long raw(long n,long a,long b,long c,long d,long e,long f) {
    long result=syscall(n,a,b,c,d,e,f);return result<0?-errno:result;
}
#include "../loader/network_identity_transport.h"

enum Mode { Normal,NonDumpable,NonBlocking,Legacy,NoRecord,BadMagic,BadPid,BadStart,
            BadUid,WrongProc,RegularFile,ExtraFds,MissingFd,BadHeader,ShortHeader,
            LegacySocket,ProofSocket,NonDumpableSocket,TooManyFds,WrongKernelUid };
static void require(bool result,const char* operation) {
    if(!result){fprintf(stderr,"FAIL %s errno=%d\n",operation,errno);exit(1);}
}
static int descriptors() {
    DIR* directory=opendir("/proc/self/fd");require(directory,"fd directory");int count=0;
    while(readdir(directory))++count;closedir(directory);return count;
}
static uid_t kernel_uid() {
    FILE* file=fopen("/proc/self/status","r");require(file,"self status");
    char text[256];unsigned uid=0;bool found=false;
    while(fgets(text,sizeof(text),file))if(sscanf(text,"Uid:\t%u",&uid)==1){found=true;break;}
    fclose(file);require(found,"kernel UID");return uid;
}
static uint64_t start_time(pid_t pid) {
    char path[64],text[4096];snprintf(path,sizeof(path),"/proc/%d/stat",pid);
    FILE* file=fopen(path,"r");require(file,"process stat");size_t count=fread(text,1,sizeof(text),file);fclose(file);
    uint64_t result=pocket_process_start(text,count);require(result,"process start");return result;
}
static void send_custom(int socket_fd,Mode mode,pid_t parent,const char* directory) {
    char path[4096];snprintf(path,sizeof(path),"/proc/%d/stat",mode==WrongProc?parent:getpid());
    int proof=open(path,O_RDONLY|O_CLOEXEC);require(proof>=0,"open proof");
    if(mode==RegularFile) {
        char text[4096];ssize_t size=read(proof,text,sizeof(text));require(size>0,"copy stat");close(proof);
        snprintf(path,sizeof(path),"%s/copied-stat",directory);proof=open(path,O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC,0600);
        require(proof>=0 && write(proof,text,size)==size,"regular stat lookalike");
    }
    unsigned char header[8];memcpy(header,pocket_network_identity_header,sizeof(header));
    if(mode==BadHeader)header[6]=99;
    unsigned count=mode==TooManyFds?8:mode==ExtraFds?2:mode==MissingFd?0:1;
    union {cmsghdr alignment;unsigned char bytes[CMSG_SPACE(sizeof(int)*8)];} control{};
    iovec part{header,mode==ShortHeader?size_t(3):sizeof(header)};
    msghdr message{};message.msg_iov=&part;message.msg_iovlen=1;
    if(count) {
        message.msg_control=control.bytes;message.msg_controllen=CMSG_SPACE(sizeof(int)*count);
        cmsghdr* item=CMSG_FIRSTHDR(&message);item->cmsg_level=SOL_SOCKET;item->cmsg_type=SCM_RIGHTS;
        item->cmsg_len=CMSG_LEN(sizeof(int)*count);
        for(unsigned i=0;i<count;++i)memcpy(CMSG_DATA(item)+sizeof(int)*i,&proof,sizeof(proof));
    }
    sendmsg(socket_fd,&message,MSG_NOSIGNAL);close(proof);
}
static void send_socket_command(int client) {
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);require(fd>=0,"internet socket");
    unsigned char command[8]={0,2,3,4,5,6,7,8};
    union {cmsghdr alignment;unsigned char bytes[CMSG_SPACE(sizeof(int))];} control{};
    iovec part{command,sizeof(command)};msghdr message{};message.msg_iov=&part;message.msg_iovlen=1;
    message.msg_control=control.bytes;message.msg_controllen=sizeof(control.bytes);
    cmsghdr* item=CMSG_FIRSTHDR(&message);item->cmsg_level=SOL_SOCKET;item->cmsg_type=SCM_RIGHTS;
    item->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(item),&fd,sizeof(fd));
    require(sendmsg(client,&message,MSG_NOSIGNAL)==sizeof(command),"send binary socket command");close(fd);
}
static void receive_socket_command(int peer) {
    unsigned char command[8]={};union {cmsghdr alignment;unsigned char bytes[CMSG_SPACE(sizeof(int))];} control{};
    iovec part{command,sizeof(command)};msghdr message{};message.msg_iov=&part;message.msg_iovlen=1;
    message.msg_control=control.bytes;message.msg_controllen=sizeof(control.bytes);
    require(recvmsg(peer,&message,MSG_CMSG_CLOEXEC)==sizeof(command) && !(message.msg_flags&(MSG_TRUNC|MSG_CTRUNC)),"binary command size");
    for(unsigned i=0;i<sizeof(command);++i)require(command[i]==(i?i+1:0),"binary command unchanged");
    cmsghdr* item=CMSG_FIRSTHDR(&message);
    require(item && item->cmsg_level==SOL_SOCKET && item->cmsg_type==SCM_RIGHTS &&
            item->cmsg_len==CMSG_LEN(sizeof(int)) && !CMSG_NXTHDR(&message,item),"command socket rights preserved");
    int fd=-1;memcpy(&fd,CMSG_DATA(item),sizeof(fd));int domain=0;socklen_t size=sizeof(domain);
    require(fd>=0 && !getsockopt(fd,SOL_SOCKET,SO_DOMAIN,&domain,&size) && domain==AF_INET,"received internet socket");close(fd);
}
static void trial(Mode mode,const char* name,bool expected) {
    int before=descriptors();char directory[]="/tmp/pocket-dns-test-XXXXXX";
    require(mkdtemp(directory),"temporary directory");
    char socket_path[108];snprintf(socket_path,sizeof(socket_path),"%s/socket",directory);
    int server=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);require(server>=0,"listener");
    sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,socket_path);
    require(!bind(server,reinterpret_cast<sockaddr*>(&address),sizeof(address)) && !listen(server,1),"listen");
    int gate[2],ready[2];require(!pipe(gate) && !pipe(ready),"gates");
    pid_t parent=getpid(),child=fork();require(child>=0,"fork");
    if(!child) {
        close(ready[0]);require(!prctl(PR_SET_DUMPABLE,1),"initial child dumpability");
        require(write(ready[1],"r",1)==1,"child ready");close(ready[1]);
        close(server);close(gate[1]);char byte=0;require(read(gate[0],&byte,1)==1,"gate read");close(gate[0]);
        require(!prctl(PR_SET_DUMPABLE,(mode==NonDumpable || mode==NonDumpableSocket)?0:1),"child dumpability");
        int client=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);require(client>=0,"client");
        require(!connect(client,reinterpret_cast<sockaddr*>(&address),sizeof(address)),"connect");
        if(mode==NonBlocking)require(fcntl(client,F_SETFL,O_NONBLOCK)==0,"nonblocking");
        if(mode==Normal || mode==NonDumpable || mode==NonBlocking || mode==ProofSocket || mode==NonDumpableSocket)require(!pocket_network_send_identity(client),"production sender");
        else if(mode==LegacySocket) { }
        else if(mode==Legacy)send(client,"getaddrinfo x",sizeof("getaddrinfo x"),MSG_NOSIGNAL);
        else send_custom(client,mode,parent,directory);
        if(mode==LegacySocket || mode==ProofSocket || mode==NonDumpableSocket)send_socket_command(client);
        else if(mode!=Legacy && mode!=ShortHeader)send(client,"getaddrinfo x",sizeof("getaddrinfo x"),MSG_NOSIGNAL);
        if(mode==NonBlocking)require(fcntl(client,F_SETFL,0)==0,"restore blocking");
        shutdown(client,SHUT_WR);recv(client,&byte,1,0);close(client);_exit(0);
    }
    close(ready[1]);char ready_byte=0;require(read(ready[0],&ready_byte,1)==1,"wait for child");close(ready[0]);
    close(gate[0]);char record_path[4096];snprintf(record_path,sizeof(record_path),"%s/%d",directory,child);
    PocketBinderIdentity record{POCKET_IDENTITY_MAGIC,static_cast<uint32_t>(child),10115,10115,start_time(child)};
    if(mode==BadMagic)record.magic=0;
    if(mode==BadPid)++record.pid;
    if(mode==BadStart)++record.start_ticks;
    if(mode==BadUid)record.uid=UINT32_MAX;
    if(mode!=NoRecord) {
        int file=open(record_path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
        require(file>=0 && write(file,&record,sizeof(record))==sizeof(record),"publish record");close(file);
    }
    require(write(gate[1],"g",1)==1,"release client");close(gate[1]);
    int peer=accept4(server,nullptr,nullptr,SOCK_CLOEXEC);require(peer>=0,"accept");
    timeval timeout{2,0};setsockopt(peer,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    uint64_t identity=UINT64_MAX;uid_t uid=kernel_uid();
    bool actual=pocket_network_receive_identity(peer,mode==WrongKernelUid?uid+1:uid,&identity,directory);
    require(actual==expected,name);
    if(expected) {
        require(identity==((uint64_t(10115)<<32)|uint32_t(child)),"scoped guest identity");
        if(mode==LegacySocket || mode==ProofSocket || mode==NonDumpableSocket)receive_socket_command(peer);
        else {char command[sizeof("getaddrinfo x")]={};
            require(recv(peer,command,sizeof(command),MSG_WAITALL)==sizeof(command) &&
                !memcmp(command,"getaddrinfo x",sizeof(command)),"command bytes untouched");}
    }else require(identity==UINT64_MAX,"failed identity unchanged");
    send(peer,"a",1,MSG_NOSIGNAL);close(peer);close(server);
    int status=0;require(waitpid(child,&status,0)==child,"wait");
    require(WIFEXITED(status) && WEXITSTATUS(status)==0,"client completion");
    unlink(record_path);unlink(socket_path);char copied[4096];snprintf(copied,sizeof(copied),"%s/copied-stat",directory);unlink(copied);
    require(!rmdir(directory),"temporary cleanup");require(descriptors()==before,"all descriptors closed");
    printf("PASS %s\n",name);fflush(stdout);
}
int main() {
    require(!prctl(PR_SET_DUMPABLE,1),"parent dumpability");
    trial(Normal,"real procfs descriptor",true);trial(NonDumpable,"nondumpable real sender",true);
    trial(NonBlocking,"nonblocking sender",true);trial(Legacy,"legacy command preserved",true);
    trial(NoRecord,"missing identity",false);trial(BadMagic,"invalid identity magic",false);
    trial(BadPid,"PID mismatch",false);trial(BadStart,"stale process start",false);
    trial(BadUid,"invalid UID range",false);trial(WrongProc,"another process descriptor",false);
    trial(RegularFile,"copied stat file",false);trial(ExtraFds,"multiple descriptors",false);
    trial(MissingFd,"missing descriptor",false);trial(BadHeader,"invalid protocol",false);
    trial(ShortHeader,"incomplete protocol",false);trial(LegacySocket,"legacy binary command and socket FD preserved",true);
    trial(ProofSocket,"proof and binary socket command separated",true);
    trial(NonDumpableSocket,"nondumpable proof and socket command",true);
    trial(TooManyFds,"truncated ancillary descriptors",false);trial(WrongKernelUid,"wrong host UID",false);
    puts("NETWORK_IDENTITY_TESTS_PASS cases=20");
}
