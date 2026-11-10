// SPDX-License-Identifier: GPL-3.0-only
// Independent client of Android 12L's real ZygoteConnection socket protocol.
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
#include <time.h>

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
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);
    const bool preloaded=argc==2 && !strcmp(argv[1],"preloaded");
    if(argc!=1 && !preloaded)return 64;
    char sdk[PROP_VALUE_MAX];__system_property_get("ro.build.version.sdk",sdk);
    if(strcmp(sdk,"32") || setgid(preloaded?1000:39999) || setuid(preloaded?1000:39999))return 64;
    char originalPid[PROP_VALUE_MAX]={};
    if(preloaded && !__system_property_get("init.svc_debug_pid.pocket-framework",originalPid))return 8;
    timespec now={};clock_gettime(CLOCK_MONOTONIC,&now);const time_t deadline=now.tv_sec+240;
    int fd=-1;
    for(int n=0;n<(preloaded?2400:200);n++) {
        if(preloaded) {
            clock_gettime(CLOCK_MONOTONIC,&now);
            if(now.tv_sec>=deadline){puts("API32_PRIMARY_ZYGOTE_PRELOAD_TIMEOUT");return 12;}
            char state[PROP_VALUE_MAX],pid[PROP_VALUE_MAX];
            __system_property_get("init.svc.pocket-framework",state);
            __system_property_get("init.svc_debug_pid.pocket-framework",pid);
            if(strcmp(state,"running") || strcmp(pid,originalPid)) {
                printf("API32_PRIMARY_ZYGOTE_PRELOAD_EXIT state=%s original_pid=%s current_pid=%s\n",state,originalPid,pid);return 9;
            }
        }
        fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
        if(fd<0)return 1;
        sockaddr_un address={};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/zygote");
        if(!connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)))break;
        close(fd);fd=-1;usleep(100000);
    }
    if(fd<0){puts("API32_ZYGOTE_CONNECT_TIMEOUT");return 2;}
    clock_gettime(CLOCK_MONOTONIC,&now);
    if(preloaded && now.tv_sec>=deadline)return 12;
    timeval timeout={preloaded?deadline-now.tv_sec:30,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    if(!abi(fd)){puts("API32_ZYGOTE_ABI_FAILED");return 3;}
    puts("API32_ZYGOTE_ABI_PASS arm64-v8a");
    if(preloaded) {
        clock_gettime(CLOCK_MONOTONIC,&now);if(now.tv_sec>=deadline)return 12;
        timeout.tv_sec=deadline-now.tv_sec;setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        char query[]="1\n--preload-default\n";uint32_t result=0;
        if(!transfer(fd,query,strlen(query),true) || !transfer(fd,&result,4,false))return 10;
        close(fd);
        // 0 means this request performed a lazy preload. A normally initialized
        // primary zygote must answer 1: preload was already complete.
        if(ntohl(result)!=1){printf("API32_PRIMARY_ZYGOTE_PRELOAD_NOT_COMPLETE result=%u\n",ntohl(result));return 11;}
        char currentPid[PROP_VALUE_MAX];__system_property_get("init.svc_debug_pid.pocket-framework",currentPid);
        if(strcmp(currentPid,originalPid))return 9;
        printf("API32_PRIMARY_ZYGOTE_PRELOAD_PASS protocol=stock preload_was_complete=1 abi=arm64-v8a pid=%s\n",originalPid);return 0;
    }
    unsigned long long random=0;
    if(getrandom(&random,sizeof(random),0)!=sizeof(random))return 4;
    char nonce[17];snprintf(nonce,sizeof(nonce),"%016llx",random);
    unlink("/data/api32-child.txt");
    char request[1024];
    snprintf(request,sizeof(request),"10\n--runtime-args\n--setuid=39999\n--setgid=39999\n--setgroups=1000,3003\n--runtime-flags=0\n--target-sdk-version=32\n--nice-name=pocket-api32-child\n--instruction-set=arm64\ndev.kreijstal.aosppocket.guest.Api32Child\n%s\n",nonce);
    uint32_t pid=0;unsigned char wrapper=0;
    if(!transfer(fd,request,strlen(request),true) || !transfer(fd,&pid,4,false) || !transfer(fd,&wrapper,1,false) ||
        static_cast<int32_t>(ntohl(pid))<=1 || wrapper){puts("API32_ZYGOTE_FORK_FAILED");return 5;}
    for(int n=0;n<300;n++) {
        FILE* file=fopen("/data/api32-child.txt","r");
        char result[512]={};bool found=file && fgets(result,sizeof(result),file);if(file)fclose(file);
        if(found && strstr(result,"API32_JAVA_CHILD_PASS uid=39999 gid=39999 sdk=32 threads=1") && strstr(result,nonce)) {
            fputs(result,stdout);
            if(!abi(fd))return 6;
            close(fd);printf("API32_ZYGOTE_PASS child_pid=%u parent_accepts_commands=1\n",ntohl(pid));return 0;
        }
        usleep(100000);
    }
    puts("API32_ZYGOTE_CHILD_TIMEOUT");return 7;
}
