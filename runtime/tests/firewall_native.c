// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../loader/firewall.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FIREWALL_NATIVE_FAIL line=%d errno=%d\n",__LINE__,errno);exit(1);}}while(0)
static void command(char* const* args) {
    pid_t child=fork();CHECK(child>=0);
    if(!child) {execv("/system/bin/iptables",args);_exit(120);}
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
}
#define POLICY(...) command((char* const[]){"iptables",__VA_ARGS__,NULL})
// Exercise an instruction-level syscall as well as the stock libc path.
static long direct_send(int fd,const void* data,size_t size,const struct sockaddr_in* address) {
    register long x0 __asm__("x0")=fd,x1 __asm__("x1")=(long)data,x2 __asm__("x2")=(long)size;
    register long x3 __asm__("x3")=0,x4 __asm__("x4")=(long)address,x5 __asm__("x5")=sizeof(*address);
    register long x8 __asm__("x8")=__NR_sendto;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x8):"memory","cc");return x0;
}
int main(void) {
    setbuf(stdout,NULL);CHECK(geteuid()==0);unlink(POCKET_FW_PATH);
    FILE* status=fopen("/proc/self/status","re");CHECK(status);char line[512];unsigned kernel_uid=0;int tracer=-1;
    while(fgets(line,sizeof(line),status)) {sscanf(line,"Uid: %u",&kernel_uid);sscanf(line,"TracerPid: %d",&tracer);}
    fclose(status);CHECK(kernel_uid>=10000 && tracer==0);
    // This isolated fixture has no guest netd. Bypass stock libnetd_client's
    // socket-creation hook; exercise real socket syscalls under our filter.
    int server=(int)syscall(SYS_socket,AF_INET,SOCK_DGRAM,0),client=(int)syscall(SYS_socket,AF_INET,SOCK_DGRAM,0);CHECK(server>=0 && client>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    CHECK(!bind(server,(struct sockaddr*)&address,sizeof(address)));socklen_t len=sizeof(address);
    CHECK(!getsockname(server,(struct sockaddr*)&address,&len));
    CHECK(!connect(client,(struct sockaddr*)&address,sizeof(address)));
    char data[]="native-firewall",got[32];CHECK(write(client,data,sizeof(data))==sizeof(data));CHECK(recv(server,got,sizeof(got),0)==sizeof(data));
    POLICY("-P","OUTPUT","DROP");
    CHECK(write(client,data,sizeof(data))==-1 && errno==EACCES);
    CHECK(direct_send(client,data,sizeof(data),&address)==-EACCES);
    struct iovec vector={data,sizeof(data)};CHECK(writev(client,&vector,1)==-1 && errno==EACCES);
    struct msghdr message={.msg_name=&address,.msg_namelen=sizeof(address),.msg_iov=&vector,.msg_iovlen=1};
    CHECK(sendmsg(client,&message,0)==-1 && errno==EACCES);
    struct mmsghdr batch={.msg_hdr=message};CHECK(sendmmsg(client,&batch,1,0)==-1 && errno==EACCES);
    int tcp=(int)syscall(SYS_socket,AF_INET,SOCK_STREAM,0);CHECK(tcp>=0);
    CHECK(connect(tcp,(struct sockaddr*)&address,sizeof(address))==-1 && errno==EACCES);close(tcp);
    // A raw write with an invalid address must remain EFAULT, not crash the handler.
    CHECK(syscall(SYS_sendto,client,data,sizeof(data),0,(void*)1,sizeof(address))==-1 && errno==EFAULT);
    POLICY("-A","OUTPUT","-p","udp","-j","ACCEPT");
    CHECK(write(client,data,sizeof(data))==sizeof(data));CHECK(recv(server,got,sizeof(got),0)==sizeof(data));
    POLICY("-F","OUTPUT");
    CHECK(write(client,data,sizeof(data))==-1 && errno==EACCES);
    int pair[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));CHECK(write(pair[0],data,sizeof(data))==sizeof(data));CHECK(read(pair[1],got,sizeof(got))==sizeof(data));close(pair[0]);close(pair[1]);
    POLICY("-P","OUTPUT","ACCEPT");
    POLICY("-A","OUTPUT","-m","owner","--uid-owner","10017","-j","DROP");
    for(unsigned uid=10017;uid<=10018;uid++) {
        pid_t child=fork();CHECK(child>=0);
        if(!child) {CHECK(!setuid(uid));ssize_t sent=write(client,data,sizeof(data));
            CHECK(uid==10017?(sent==-1 && errno==EACCES):sent==sizeof(data));_exit(0);}
        int result;CHECK(waitpid(child,&result,0)==child && WIFEXITED(result) && WEXITSTATUS(result)==0);
    }
    CHECK(recv(server,got,sizeof(got),0)==sizeof(data));
    POLICY("-6","-P","OUTPUT","DROP");
    int ipv6=(int)syscall(SYS_socket,AF_INET6,SOCK_DGRAM,0);CHECK(ipv6>=0);
    struct sockaddr_in6 v6={.sin6_family=AF_INET6,.sin6_addr=IN6ADDR_LOOPBACK_INIT,.sin6_port=htons(12345)};
    CHECK(sendto(ipv6,data,sizeof(data),0,(struct sockaddr*)&v6,sizeof(v6))==-1 && errno==EACCES);close(ipv6);
    POLICY("-F","OUTPUT");POLICY("-P","OUTPUT","DROP");
    close(server);close(client);
    printf("NATIVE_FIREWALL_PASS kernelUid=%u guestUid=%u tracerPid=%d directSvc=blocked connectedWrite=blocked unixIpc=working uidMatch=working ipv6=blocked\n",kernel_uid,geteuid(),tracer);
    return 0;
}
