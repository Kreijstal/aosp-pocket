// SPDX-License-Identifier: GPL-3.0-only
// Explicit offline network-service profile. Never configures host networking.
#include <binder/Binder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <binder/ProcessState.h>
#include <binder/Status.h>
#include <utils/String8.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <map>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <string>
#include <unistd.h>
#include "boot_storage_protocol.h"
using namespace android;
namespace {
class NetdOffline final : public BBinder {
    status_t onTransact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags) override {
        if(code>=IBinder::LAST_CALL_TRANSACTION)return BBinder::onTransact(code,data,reply,flags);
        auto uid=IPCThreadState::self()->getCallingUid();if(uid!=0 && uid!=1000)return PERMISSION_DENIED;
        if(!reply || !data.enforceInterface(String16("android.net.INetd")))return BAD_VALUE;
        if(code==INetd_isAlive) {reply->writeInt32(0);reply->writeInt32(1);return NO_ERROR;}
        if(code==INetd_firewallReplaceUidChain || code==INetd_bandwidthEnableDataSaver) {
            reply->writeInt32(0);reply->writeInt32(0);return NO_ERROR;
        }
        fprintf(stderr,"DIRECT_NETWORK_UNSUPPORTED binder_transaction=%u\n",code);
        return binder::Status::fromServiceSpecificError(ENOTSUP,"Offline guest network profile").writeToParcel(reply);
    }
};
bool sendReply(int fd,int code,unsigned sequence,const char* text) {
    char response[256];int size=snprintf(response,sizeof(response),"%d %u %s",code,sequence,text);
    return size>0 && size<(int)sizeof(response) && send(fd,response,size+1,MSG_NOSIGNAL)==size+1;
}
void serve(int fd) {
    std::string pending;char data[2048];
    for(;;) {
        ssize_t count=read(fd,data,sizeof(data));if(count<=0)break;
        pending.append(data,count);if(pending.size()>8192)break;
        size_t end;
        while((end=pending.find('\0'))!=std::string::npos) {
            std::string request=pending.substr(0,end);pending.erase(0,end+1);
            char* tail=nullptr;errno=0;unsigned long seq=strtoul(request.c_str(),&tail,10);
            if(errno || tail==request.c_str() || *tail!=' ' || seq>UINT32_MAX)return;
            const char* command=tail+1;int code=400;const char* result="Operation unsupported in offline guest";
            if(!strcmp(command,"interface list") || !strcmp(command,"tether interface list")) {code=200;result="Interface list completed";}
            else if(!strcmp(command,"ipfwd status")) {code=211;result="Forwarding disabled";}
            else if(!strcmp(command,"tether status")) {code=210;result="Tethering services stopped";}
            else if(!strcmp(command,"clatd status")) {code=223;result="Clatd stopped";}
            // Log just the command category, never SSIDs, addresses or secrets.
            char category[32]={0};size_t n=0;
            while(command[n]>='a' && command[n]<='z' && n<sizeof(category)-1) {category[n]=command[n];++n;}
            fprintf(stderr,"DIRECT_NETWORK_COMMAND category=%s result=%d profile=offline\n",category,code);
            if(!sendReply(fd,code,seq,result))return;
        }
    }
}
void priorityHints(int fd) {
    // Android 9 ProcessList's one-way, network-byte-order LMK protocol.
    // These are guest scheduling hints, not changes to host OOM policy.
    std::map<int,std::pair<int,int>> priorities;
    uint32_t packet[32];
    for(;;) {
        ssize_t count=recv(fd,packet,sizeof(packet),MSG_TRUNC);
        if(count<=0 || count>(ssize_t)sizeof(packet) || count%4)break;
        size_t words=count/4;for(size_t n=0;n<words;++n)packet[n]=ntohl(packet[n]);
        if(packet[0]==0 && words>=3 && words<=13 && words%2==1)continue;
        if(packet[0]==2 && words==2) {priorities.erase(packet[1]);continue;}
        if(packet[0]!=1 || words!=4 || !packet[1])break;
        int pid=packet[1],uid=packet[2],adj=static_cast<int32_t>(packet[3]);
        if(pid<0 || uid<0 || adj< -1000 || adj>1000)break;
        auto value=std::make_pair(uid,adj);auto old=priorities.find(pid);
        if(old==priorities.end() || old->second!=value) {
            priorities[pid]=value;
            fprintf(stderr,"DIRECT_PRIORITY_HINT pid=%d guest_uid=%d adj=%d host_oom=unchanged\n",pid,uid,adj);
        }
    }
}
void acceptLoop(int server,bool priority) {
    for(;;) {
        int client=accept4(server,nullptr,nullptr,SOCK_CLOEXEC);
        if(client<0) {if(errno==EINTR)continue;break;}
        if(priority)priorityHints(client);else serve(client);close(client);
    }
}
int controlSocket(const char* path,int type=SOCK_STREAM) {
    int fd=socket(AF_UNIX,type|SOCK_CLOEXEC,0);if(fd<0)return -1;
    sockaddr_un address{};address.sun_family=AF_UNIX;snprintf(address.sun_path,sizeof(address.sun_path),"/dev/socket/%s",path);
    unlink(address.sun_path);
    if(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || listen(fd,4)) {close(fd);return -1;}
    return fd;
}
}
int main() {
    setbuf(stdout,nullptr);if(getuid()!=0)return 64;
    int server=controlSocket("netd"),mdns=controlSocket("mdns"),lmkd=controlSocket("lmkd",SOCK_SEQPACKET);
    if(server<0 || mdns<0 || lmkd<0) {perror("private control socket");return 1;}
    auto manager=defaultServiceManager();if(!manager || manager->addService(String16("netd"),new NetdOffline)!=NO_ERROR)return 2;
    ProcessState::self()->startThreadPool();std::thread(acceptLoop,server,false).detach();std::thread(acceptLoop,mdns,false).detach();std::thread(acceptLoop,lmkd,true).detach();
    puts("DIRECT_NETWORK_READY profile=offline interfaces=none host_network=unchanged");
    IPCThreadState::self()->joinThreadPool();return 0;
}
