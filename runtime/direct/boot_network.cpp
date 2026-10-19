// SPDX-License-Identifier: GPL-3.0-only
// Private guest network bookkeeping and host-delegated DNS. Never configures
// host interfaces, routes, marks or firewall rules; sockets retain host policy.
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
#include <mutex>
#include <set>
#include <vector>
#include <sstream>
#include <netdb.h>
#include <sys/time.h>
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
std::set<int> networks;
std::mutex networkMutex;
class HostNetd final : public BBinder {
    status_t onTransact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags) override {
        if(code>=IBinder::LAST_CALL_TRANSACTION)return BBinder::onTransact(code,data,reply,flags);
        auto uid=IPCThreadState::self()->getCallingUid();if(uid!=0 && uid!=1000)return PERMISSION_DENIED;
        if(!reply || !data.enforceInterface(String16("android.net.INetd")))return BAD_VALUE;
        if(code==INetd_isAlive) {reply->writeInt32(0);reply->writeInt32(1);return NO_ERROR;}
        if(code==INetd_firewallReplaceUidChain || code==INetd_bandwidthEnableDataSaver) {
            reply->writeInt32(0);reply->writeInt32(0);return NO_ERROR;
        }
        if(code==INetd_networkCreatePhysical || code==INetd_networkDestroy ||
           code==INetd_networkAddInterface || code==INetd_networkRemoveInterface ||
           code==INetd_setResolverConfiguration) {
            int netid=data.readInt32();
            if(netid<100 || netid>65535)return binder::Status::fromServiceSpecificError(EINVAL,"Invalid guest network ID").writeToParcel(reply);
            std::lock_guard<std::mutex> lock(networkMutex);
            if(code==INetd_networkCreatePhysical)networks.insert(netid);
            else if(code==INetd_networkDestroy)networks.erase(netid);
            else if(!networks.count(netid))return binder::Status::fromServiceSpecificError(ENONET,"Unknown guest network").writeToParcel(reply);
            reply->writeInt32(0);return NO_ERROR;
        }
        fprintf(stderr,"DIRECT_NETWORK_UNSUPPORTED binder_transaction=%u\n",code);
        return binder::Status::fromServiceSpecificError(ENOTSUP,"Host-owned network operation unavailable to the guest").writeToParcel(reply);
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
            const char* command=tail+1;int code=400;const char* result="Operation unavailable in host-delegated guest";
            if(!strcmp(command,"interface list") || !strcmp(command,"tether interface list")) {code=200;result="Interface list completed";}
            else if(!strcmp(command,"ipfwd status")) {code=211;result="Forwarding disabled";}
            else if(!strcmp(command,"tether status")) {code=210;result="Tethering services stopped";}
            else if(!strcmp(command,"clatd status")) {code=223;result="Clatd stopped";}
            else if(!strncmp(command,"network default ",16) || !strncmp(command,"resolver setnetdns ",19) ||
                    !strncmp(command,"resolver clearnetdns ",21)) {code=200;result="Guest network metadata accepted; host uplink retained";}
            else if(!strncmp(command,"network create ",15) || !strncmp(command,"network destroy ",16)) {
                std::istringstream args(command);std::string category,verb;int netid=0;args>>category>>verb>>netid;
                if(netid>=100 && netid<=65535) {
                    std::lock_guard<std::mutex> lock(networkMutex);
                    if(verb=="create")networks.insert(netid);else networks.erase(netid);
                    code=200;result="Private guest network updated";
                }
            } else if(!strncmp(command,"network interface ",18)) {
                std::istringstream args(command);std::string category,object,verb,iface;int netid=0;
                args>>category>>object>>verb>>netid>>iface;
                std::lock_guard<std::mutex> lock(networkMutex);
                if(networks.count(netid) && iface=="pocket0" && (verb=="add" || verb=="remove")) {
                    code=200;result="Private guest interface metadata updated";
                }
            }
            // Log just the command category, never SSIDs, addresses or secrets.
            char category[32]={0};size_t n=0;
            while(command[n]>='a' && command[n]<='z' && n<sizeof(category)-1) {category[n]=command[n];++n;}
            fprintf(stderr,"DIRECT_NETWORK_COMMAND category=%s result=%d profile=host_delegated\n",category,code);
            if(!sendReply(fd,code,seq,result))return;
        }
    }
}
bool transfer(int fd,void* data,size_t size,bool writing) {
    char* p=static_cast<char*>(data);
    while(size) {
        ssize_t n=writing?send(fd,p,size,MSG_NOSIGNAL):read(fd,p,size);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)return false;p+=n;size-=n;
    }
    return true;
}
bool number(int fd,uint32_t value) {value=htonl(value);return transfer(fd,&value,4,true);}
bool dataBlock(int fd,const void* data,uint32_t size) {
    return number(fd,size) && transfer(fd,const_cast<void*>(data),size,true);
}
bool hostAddresses(const std::string& hostname,std::vector<std::vector<uint8_t>>& addresses) {
    if(hostname.empty() || hostname.size()>253)return false;
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return false;
    timeval timeout{10,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/pocket_host");
    uint32_t status=0,size=0;
    bool ok=connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 &&
        number(fd,0x504b4831) && number(fd,2) && dataBlock(fd,hostname.data(),hostname.size()) &&
        transfer(fd,&status,4,false) && transfer(fd,&size,4,false);
    size=ntohl(size);status=ntohl(status);
    std::vector<uint8_t> payload;
    if(ok && !status && size>=4 && size<=2048){payload.resize(size);ok=transfer(fd,payload.data(),size,false);}else ok=false;
    close(fd);if(!ok)return false;
    size_t offset=0;
    auto get=[&](uint32_t& n) {if(offset+4>payload.size())return false;memcpy(&n,payload.data()+offset,4);offset+=4;n=ntohl(n);return true;};
    uint32_t count;if(!get(count) || count>64)return false;
    for(uint32_t i=0;i<count;i++) {
        uint32_t length;if(!get(length) || (length!=4 && length!=16) || offset+length>payload.size())return false;
        addresses.emplace_back(payload.begin()+offset,payload.begin()+offset+length);offset+=length;
    }
    return offset==payload.size() && !addresses.empty();
}
void dnsFailure(int fd,int error) {
    char code[]="401";transfer(fd,code,sizeof(code),true);transfer(fd,&error,sizeof(error),true);
}
void serveDns(int fd) {
    std::string request;char c;
    while(request.size()<2048 && read(fd,&c,1)==1 && c)request+=c;
    std::istringstream input(request);std::string command,hostname,service,extra;
    int flags,family,type,protocol;unsigned netid;
    if(!(input>>command>>hostname>>service>>flags>>family>>type>>protocol>>netid) || input>>extra || command!="getaddrinfo") {dnsFailure(fd,EAI_FAIL);return;}
    if(family==-1)family=AF_UNSPEC;if(type==-1)type=0;if(protocol==-1)protocol=0;if(flags==-1)flags=0;
    if(family!=AF_UNSPEC && family!=AF_INET && family!=AF_INET6) {dnsFailure(fd,EAI_FAMILY);return;}
    if(type!=0 && type!=SOCK_STREAM && type!=SOCK_DGRAM) {dnsFailure(fd,EAI_SOCKTYPE);return;}
    unsigned port=0;
    if(service!="^") {
        char* end=nullptr;errno=0;unsigned long value=strtoul(service.c_str(),&end,10);
        if(errno || !end || *end || value>65535) {dnsFailure(fd,EAI_SERVICE);return;}port=value;
    }
    std::vector<std::vector<uint8_t>> addresses;
    if(hostname=="^") {
        addresses.push_back({0,0,0,0});addresses.push_back(std::vector<uint8_t>(16));
        if(!(flags&AI_PASSIVE)){addresses[0]={127,0,0,1};addresses[1][15]=1;}
    } else if(!hostAddresses(hostname,addresses)) {dnsFailure(fd,EAI_NONAME);return;}
    unsigned records=0;
    for(const auto& address:addresses)if(family==AF_UNSPEC || family==(address.size()==4?AF_INET:AF_INET6))++records;
    if(!records){dnsFailure(fd,EAI_NONAME);return;}
    char code[]="222";if(!transfer(fd,code,sizeof(code),true))return;
    for(const auto& address:addresses) {
        int af=address.size()==4?AF_INET:AF_INET6;if(family!=AF_UNSPEC && family!=af)continue;
        sockaddr_storage storage{};socklen_t length;
        if(af==AF_INET) {
            auto* ip=reinterpret_cast<sockaddr_in*>(&storage);ip->sin_family=AF_INET;ip->sin_port=htons(port);
            memcpy(&ip->sin_addr,address.data(),4);length=sizeof(*ip);
        } else {
            auto* ip=reinterpret_cast<sockaddr_in6*>(&storage);ip->sin6_family=AF_INET6;ip->sin6_port=htons(port);
            memcpy(&ip->sin6_addr,address.data(),16);length=sizeof(*ip);
        }
        for(int socketType:{SOCK_STREAM,SOCK_DGRAM}) {
            if(type && type!=socketType)continue;
            int p=socketType==SOCK_STREAM?IPPROTO_TCP:IPPROTO_UDP;if(protocol && protocol!=p)continue;
            if(!number(fd,1) || !number(fd,flags) || !number(fd,af) || !number(fd,socketType) ||
                !number(fd,p) || !dataBlock(fd,&storage,length) || !number(fd,0))return;
        }
    }
    number(fd,0);
}
void serveMark(int fd) {
    // API28 FwmarkCommand, followed optionally by FwmarkConnectInfo. Received
    // descriptors are closed; SO_MARK and host VPN protection are never changed.
    uint32_t command[32]{};alignas(cmsghdr) char control[CMSG_SPACE(8*sizeof(int))]{};
    iovec buffer{command,sizeof(command)};msghdr message{};message.msg_iov=&buffer;message.msg_iovlen=1;
    message.msg_control=control;message.msg_controllen=sizeof(control);
    ssize_t count=recvmsg(fd,&message,MSG_CMSG_CLOEXEC);
    for(cmsghdr* h=CMSG_FIRSTHDR(&message);h;h=CMSG_NXTHDR(&message,h))
        if(h->cmsg_level==SOL_SOCKET && h->cmsg_type==SCM_RIGHTS && h->cmsg_len>=CMSG_LEN(0)) {
            size_t n=(h->cmsg_len-CMSG_LEN(0))/sizeof(int);auto* fds=reinterpret_cast<int*>(CMSG_DATA(h));
            for(size_t i=0;i<n;i++)close(fds[i]);
        }
    int result=-ENOTSUP;
    if(count>=16 && !(message.msg_flags&(MSG_TRUNC|MSG_CTRUNC))) {
        // ON_ACCEPT, ON_CONNECT, SELECT_NETWORK and ON_CONNECT_COMPLETE select
        // the shared host-default path. No request can bypass host VPN policy.
        if(command[0]==0 || command[0]==1 || command[0]==2 || command[0]==6)result=0;
    }
    transfer(fd,&result,sizeof(result),true);
}
void relayLoop(int server,bool dns) {
    for(;;) {
        int client=accept4(server,nullptr,nullptr,SOCK_CLOEXEC);
        if(client<0){if(errno==EINTR)continue;break;}
        timeval timeout{12,0};setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        if(dns)serveDns(client);else serveMark(client);close(client);
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
    int dns=controlSocket("dnsproxyd"),mark=controlSocket("fwmarkd");
    if(server<0 || mdns<0 || lmkd<0 || dns<0 || mark<0) {perror("private control socket");return 1;}
    auto manager=defaultServiceManager();if(!manager || manager->addService(String16("netd"),new HostNetd)!=NO_ERROR)return 2;
    ProcessState::self()->startThreadPool();std::thread(acceptLoop,server,false).detach();std::thread(acceptLoop,mdns,false).detach();std::thread(acceptLoop,lmkd,true).detach();
    std::thread(relayLoop,dns,true).detach();std::thread(relayLoop,mark,false).detach();
    puts("DIRECT_NETWORK_READY profile=host_delegated host_routes_firewall_marks=unchanged");
    IPCThreadState::self()->joinThreadPool();return 0;
}
