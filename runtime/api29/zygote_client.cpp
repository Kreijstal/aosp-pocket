// SPDX-License-Identifier: GPL-3.0-only
// Independent client of Android 10's real ZygoteConnection socket protocol.
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/random.h>
#include <sys/system_properties.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool transfer(int fd,void* bytes,size_t size,bool send) {
    char* next=static_cast<char*>(bytes);
    while(size) {
        ssize_t count=send?write(fd,next,size):read(fd,next,size);
        if(count<0 && errno==EINTR)continue;
        if(count<=0)return false;
        next+=count;size-=count;
    }
    return true;
}
static bool abi(int fd) {
    char query[]="1\n--query-abi-list\n";
    uint32_t size=0;char value[10]={};
    return transfer(fd,query,strlen(query),true) && transfer(fd,&size,4,false) && ntohl(size)==9 &&
        transfer(fd,value,9,false) && !strcmp(value,"arm64-v8a");
}
int main() {
    setbuf(stdout,nullptr);
    char sdk[PROP_VALUE_MAX];__system_property_get("ro.build.version.sdk",sdk);
    if(strcmp(sdk,"29") || setgid(39999) || setuid(39999))return 64;
    int fd=-1;
    for(int n=0;n<200;n++) {
        fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
        if(fd<0)return 1;
        sockaddr_un address={};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/zygote");
        if(!connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)))break;
        close(fd);fd=-1;usleep(100000);
    }
    if(fd<0){puts("API29_ZYGOTE_CONNECT_TIMEOUT");return 2;}
    timeval timeout={30,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    if(!abi(fd)){puts("API29_ZYGOTE_ABI_FAILED");return 3;}
    puts("API29_ZYGOTE_ABI_PASS arm64-v8a");
    unsigned long long random=0;
    if(getrandom(&random,sizeof(random),0)!=sizeof(random))return 4;
    char nonce[17];snprintf(nonce,sizeof(nonce),"%016llx",random);
    unlink("/data/api29-child.txt");
    char request[1024];
    snprintf(request,sizeof(request),"9\n--runtime-args\n--setuid=39999\n--setgid=39999\n--runtime-flags=0\n--target-sdk-version=29\n--nice-name=pocket-api29-child\n--instruction-set=arm64\ndev.kreijstal.aosppocket.guest.Api29Child\n%s\n",nonce);
    uint32_t pid=0;unsigned char wrapper=0;
    if(!transfer(fd,request,strlen(request),true) || !transfer(fd,&pid,4,false) || !transfer(fd,&wrapper,1,false) ||
        static_cast<int32_t>(ntohl(pid))<=1 || wrapper){puts("API29_ZYGOTE_FORK_FAILED");return 5;}
    for(int n=0;n<300;n++) {
        FILE* file=fopen("/data/api29-child.txt","r");
        char result[512]={};bool found=file && fgets(result,sizeof(result),file);if(file)fclose(file);
        if(found && strstr(result,"API29_JAVA_CHILD_PASS uid=39999 gid=39999 sdk=29 threads=1") && strstr(result,nonce)) {
            fputs(result,stdout);
            if(!abi(fd))return 6;
            close(fd);printf("API29_ZYGOTE_PASS child_pid=%u parent_accepts_commands=1\n",ntohl(pid));return 0;
        }
        usleep(100000);
    }
    puts("API29_ZYGOTE_CHILD_TIMEOUT");return 7;
}
