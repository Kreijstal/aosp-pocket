// SPDX-License-Identifier: GPL-3.0-only
// Private guest network bookkeeping and host-delegated DNS. Never configures
// host interfaces, routes, marks or firewall rules; sockets retain host policy.
#ifndef POCKET_GUEST_API
#define POCKET_GUEST_API 28
#endif
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
#include <algorithm>
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
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include "boot_storage_protocol.h"
#include "network_permissions.h"
#if POCKET_GUEST_API == 29
#include "../loader/network_policy.h"
#include "../api28/binder_identity.h"
#include "kernel_identity.h"
#include <sys/wait.h>
#include <sys/syscall.h>
#endif
using namespace android;
namespace {
std::set<int> networks;
std::mutex networkMutex;
pocket::NetworkPermissions permissions;
#if POCKET_GUEST_API == 29
PocketNetworkPolicy networkPolicy;
int publishPolicy(const PocketNetworkPolicy& next) {
    std::string temporary=std::string(POCKET_NP_PATH)+".tmp."+std::to_string(getpid());
    unlink(temporary.c_str());int fd=open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return errno;
    const char* p=reinterpret_cast<const char*>(&next);size_t left=sizeof(next);int error=0;
    while(left){ssize_t n=write(fd,p,left);if(n<0 && errno==EINTR)continue;
        if(n<=0){error=n<0?errno:EIO;break;}p+=n;left-=n;}
    if(!error && fsync(fd))error=errno;
    if(close(fd) && !error)error=errno;
    if(!error && rename(temporary.c_str(),POCKET_NP_PATH))error=errno;
    if(error){unlink(temporary.c_str());return error;}
    networkPolicy=next;return 0;
}
status_t policyError(Parcel* reply,int error) {
    return binder::Status::fromServiceSpecificError(error,"Guest UID network policy could not be applied").writeToParcel(reply);
}
#endif
class HostNetd final : public BBinder {
    std::vector<sp<IBinder>> listeners;
    status_t onTransact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags) override {
        if(code>=IBinder::LAST_CALL_TRANSACTION && !(POCKET_GUEST_API==29 && code==16777215))return BBinder::onTransact(code,data,reply,flags);
        auto uid=IPCThreadState::self()->getCallingUid();
        bool networkStack=false;
#if POCKET_GUEST_API == 29
        networkStack=uid==1073 && (code==INetd_isAlive || code==16777215 || code==INetd_registerUnsolicitedEventListener);
#endif
        if(uid!=0 && uid!=1000 && !networkStack)return PERMISSION_DENIED;
        if(!reply || !data.enforceInterface(String16("android.net.INetd")))return BAD_VALUE;
#if POCKET_GUEST_API == 29
        if(code==16777215){reply->writeInt32(0);reply->writeInt32(2);return NO_ERROR;}
        if(code==INetd_registerUnsolicitedEventListener){
            auto value=data.readStrongBinder();if(!value)return BAD_VALUE;
            std::lock_guard<std::mutex> lock(networkMutex);
            if(std::find(listeners.begin(),listeners.end(),value)==listeners.end()){
                if(listeners.size()>=32)return NO_MEMORY;listeners.push_back(value);
            }
            reply->writeInt32(0);return NO_ERROR;
        }
        if(code==INetd_interfaceGetList){reply->writeInt32(0);reply->writeInt32(0);return NO_ERROR;}
        if(code==INetd_firewallSetFirewallType){
            // The base chain stays default-allow. Child chains and INTERNET
            // permissions are independently enforced by the private loader.
            if(data.readInt32()!=1)return policyError(reply,ENOTSUP);
            reply->writeInt32(0);return NO_ERROR;
        }
        if(code==INetd_trafficSetNetPermForUids){
            int permission=data.readInt32();std::vector<int32_t> ids;
            if((permission!=-1 && (permission<0 || (permission&~12))) ||
                data.readInt32Vector(&ids)!=NO_ERROR || ids.size()>POCKET_NP_UIDS)return BAD_VALUE;
            for(int id:ids)if(id<0 || static_cast<unsigned>(id)>=POCKET_NP_UIDS)return BAD_VALUE;
            std::lock_guard<std::mutex> lock(networkMutex);PocketNetworkPolicy next=networkPolicy;
            next.header.enabled|=1U<<POCKET_NP_NO_INTERNET;
            for(int id:ids){
                auto& byte=next.members[POCKET_NP_NO_INTERNET][id/8];
                if(permission!=-1 && !(permission&4))byte|=1U<<(id%8);else byte&=~(1U<<(id%8));
            }
            int error=publishPolicy(next);if(error)return policyError(reply,error);
            reply->writeInt32(0);return NO_ERROR;
        }
        if(code==INetd_firewallReplaceUidChain || code==INetd_firewallEnableChildChain || code==INetd_firewallSetUidRule){
            std::lock_guard<std::mutex> lock(networkMutex);PocketNetworkPolicy next=networkPolicy;
            if(code==INetd_firewallReplaceUidChain){
                String16 name=data.readString16();int whitelist=data.readInt32();int chain=-1;
                if(name==String16("fw_dozable"))chain=POCKET_NP_DOZE;
                else if(name==String16("fw_standby"))chain=POCKET_NP_STANDBY;
                else if(name==String16("fw_powersave"))chain=POCKET_NP_POWERSAVE;
                if(chain<0 || whitelist!=(chain!=POCKET_NP_STANDBY))return policyError(reply,EINVAL);
                std::vector<int32_t> ids;if(data.readInt32Vector(&ids)!=NO_ERROR)return BAD_VALUE;
                for(int uid:ids)if(uid<0 || static_cast<unsigned>(uid)>=POCKET_NP_UIDS)return policyError(reply,EINVAL);
                memset(next.members[chain],0,POCKET_NP_BYTES);
                for(int uid:ids)next.members[chain][uid/8]|=1U<<(uid%8);
            }else{
                int chain=data.readInt32()-1;if(chain<0 || chain>POCKET_NP_POWERSAVE)return policyError(reply,ENOTSUP);
                if(code==INetd_firewallEnableChildChain){
                    int enabled=data.readInt32();if(enabled!=0 && enabled!=1)return BAD_VALUE;
                    if(enabled)next.header.enabled|=1U<<chain;else next.header.enabled&=~(1U<<chain);
                }else{
                    int uid=data.readInt32(),rule=data.readInt32();
                    if(uid<0 || static_cast<unsigned>(uid)>=POCKET_NP_UIDS || (rule!=1 && rule!=2))return policyError(reply,EINVAL);
                    bool member=chain==POCKET_NP_STANDBY?rule==2:rule==1;
                    if(member)next.members[chain][uid/8]|=1U<<(uid%8);else next.members[chain][uid/8]&=~(1U<<(uid%8));
                }
            }
            int error=publishPolicy(next);if(error)return policyError(reply,error);
            reply->writeInt32(0);if(code==INetd_firewallReplaceUidChain)reply->writeInt32(1);
            fprintf(stderr,"API29_NETWORK_POLICY_APPLIED operation=%u enabled=%u host_firewall=unchanged\n",code,next.header.enabled);
            return NO_ERROR;
        }
        if(code==INetd_bandwidthEnableDataSaver){
            if(data.readInt32())return policyError(reply,ENOTSUP);
            reply->writeInt32(0);reply->writeInt32(1);return NO_ERROR;
        }
        if(code==INetd_networkSetPermissionForUser || code==INetd_networkClearPermissionForUser){
            bool setting=code==INetd_networkSetPermissionForUser;
            int permission=setting?data.readInt32():0;
            std::vector<int32_t> ids;if(data.readInt32Vector(&ids)!=NO_ERROR || ids.size()>65536)return BAD_VALUE;
            if(setting && permission!=1 && permission!=2)return BAD_VALUE;
            std::ostringstream command;command<<"network permission user "<<(setting?"set ":"clear ");
            if(setting)command<<(permission==1?"NETWORK ":"SYSTEM ");
            for(int uid:ids){if(uid<0)return BAD_VALUE;command<<uid<<' ';}
            std::lock_guard<std::mutex> lock(networkMutex);
            if(!ids.empty() && !permissions.apply(command.str()))return BAD_VALUE;
            reply->writeInt32(0);return NO_ERROR;
        }
#endif
        if(code==INetd_isAlive) {reply->writeInt32(0);reply->writeInt32(1);return NO_ERROR;}
        if(code==INetd_firewallReplaceUidChain || code==INetd_bandwidthEnableDataSaver) {
            // Manual OUTPUT rules are enforced by the native interpreter.
            // Android-managed chains/data saver have no implementation yet;
            // never report success for a policy that was not applied.
            return binder::Status::fromServiceSpecificError(ENOTSUP,"Guest netd-managed firewall chains are not implemented").writeToParcel(reply);
        }
        if(code==INetd_networkCreatePhysical || code==INetd_networkDestroy ||
           code==INetd_networkAddInterface || code==INetd_networkRemoveInterface
#if POCKET_GUEST_API == 28
           || code==INetd_setResolverConfiguration
#endif
           ) {
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
#if POCKET_GUEST_API == 29
// Android 10 split resolver management from INetd. DNS queries use the existing
// host relay socket; arbitrary DNS/TLS configuration remains unsupported.
class HostResolver final : public BBinder {
    std::vector<sp<IBinder>> listeners;
    std::set<int> caches;
    status_t onTransact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags) override {
        if(code>=IBinder::LAST_CALL_TRANSACTION && code!=16777215)return BBinder::onTransact(code,data,reply,flags);
        auto uid=IPCThreadState::self()->getCallingUid();if(uid!=0 && uid!=1000)return PERMISSION_DENIED;
        if(!reply || !data.enforceInterface(String16("android.net.IDnsResolver")))return BAD_VALUE;
        if(code==16777215 || code==IDnsResolver_isAlive){
            reply->writeInt32(0);reply->writeInt32(code==16777215?2:1);return NO_ERROR;
        }
        if(code==IDnsResolver_registerEventListener){
            auto value=data.readStrongBinder();if(!value)return BAD_VALUE;
            std::lock_guard<std::mutex> lock(networkMutex);
            if(std::find(listeners.begin(),listeners.end(),value)==listeners.end()){
                if(listeners.size()>=32)return NO_MEMORY;listeners.push_back(value);
            }
            reply->writeInt32(0);return NO_ERROR;
        }
        if(code==IDnsResolver_createNetworkCache || code==IDnsResolver_destroyNetworkCache){
            int id=data.readInt32();if(id<100 || id>65535)return BAD_VALUE;
            std::lock_guard<std::mutex> lock(networkMutex);
            if(code==IDnsResolver_createNetworkCache)caches.insert(id);else caches.erase(id);
            reply->writeInt32(0);return NO_ERROR;
        }
        return binder::Status::fromServiceSpecificError(ENOTSUP,
            "DNS configuration is owned by the host; guest resolver customization is unsupported").writeToParcel(reply);
    }
};
#endif
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
            else if(!strncmp(command,"network permission user ",24)) {
                std::lock_guard<std::mutex> lock(networkMutex);
                if(permissions.apply(command)) {
                    code=200;result="Guest UID permission metadata updated; host policy retained";
                } else result="Invalid guest UID permission request";
            }
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
#if POCKET_GUEST_API == 29
    if(hostname!="^" && hostname!="localhost"){
        ucred peer{};socklen_t length=sizeof(peer);uint64_t identity=0;bool denied=true;
        if(!getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&length) && peer.uid==pocket_kernel_uid_from_proc() && pocket_binder_identity(peer.pid,&identity)){
            std::lock_guard<std::mutex> lock(networkMutex);denied=pocket_np_denies(&networkPolicy,identity>>32);
        }
        if(denied){dnsFailure(fd,EAI_FAIL);return;}
    }
#endif
    if(hostname=="^") {
        addresses.push_back({0,0,0,0});addresses.push_back(std::vector<uint8_t>(16));
        if(!(flags&AI_PASSIVE)){addresses[0]={127,0,0,1};addresses[1][15]=1;}
    } else if(hostname=="localhost") {
        addresses.push_back({127,0,0,1});addresses.push_back(std::vector<uint8_t>(16));addresses[1][15]=1;
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
#if POCKET_GUEST_API == 29
        if(packet[0]==3 && words==1){priorities.clear();continue;}
        if(packet[0]==4 && words==3){
            int min=static_cast<int32_t>(packet[1]),max=static_cast<int32_t>(packet[2]);
            if(min< -1000 || max>1000 || min>max)break;
            // This adapter records hints and never kills processes. The host's
            // LMK owns memory policy and has no guest-UID accounting here.
            uint32_t reply[2]={htonl(4),0};
            if(send(fd,reply,sizeof(reply),MSG_NOSIGNAL)!=(ssize_t)sizeof(reply))break;
            continue;
        }
#endif
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
#if POCKET_GUEST_API == 29
int policyPacket(int argc,char** argv){
    if(argc!=4 || getuid()!=99998)return 64;
    sockaddr_in address{};address.sin_family=AF_INET;
    char* end=nullptr;long port=strtol(argv[3],&end,10);
    if(!end || *end || port<1 || port>65535 || inet_pton(AF_INET,argv[2],&address.sin_addr)!=1)return 65;
    address.sin_port=htons(port);int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd<0)return 66;
    ssize_t sent=sendto(fd,"p",1,MSG_NOSIGNAL,reinterpret_cast<sockaddr*>(&address),sizeof(address));int error=errno;
    close(fd);return !strcmp(argv[1],"policy-denied")?(sent==-1 && error==EACCES?0:67):(sent==1?0:68);
}
int verifyPolicy(const sp<IServiceManager>& manager,const char* executable){
    auto service=manager->getService(String16("netd"));if(!service)return 10;
    auto replace=[&](bool include){
        Parcel data,reply;data.writeInterfaceToken(String16("android.net.INetd"));
        data.writeString16(String16("fw_standby"));data.writeInt32(0);
        data.writeInt32Vector(include?std::vector<int32_t>{99998}:std::vector<int32_t>{});
        return service->transact(INetd_firewallReplaceUidChain,data,&reply)==NO_ERROR && reply.readInt32()==0 && reply.readInt32()==1;
    };
    auto enable=[&](bool value){
        Parcel data,reply;data.writeInterfaceToken(String16("android.net.INetd"));data.writeInt32(2);data.writeInt32(value);
        return service->transact(INetd_firewallEnableChildChain,data,&reply)==NO_ERROR && reply.readInt32()==0;
    };
    auto internet=[&](int permission){
        Parcel data,reply;data.writeInterfaceToken(String16("android.net.INetd"));
        data.writeInt32(permission);data.writeInt32Vector(std::vector<int32_t>{99998});
        return service->transact(INetd_trafficSetNetPermForUids,data,&reply)==NO_ERROR && reply.readInt32()==0;
    };
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(9);inet_pton(AF_INET,"192.0.2.1",&address.sin_addr);
    int route=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);socklen_t length=sizeof(address);
    if(route<0){
        perror("API29_POLICY_SOCKET");
        int raw=static_cast<int>(syscall(__NR_socket,AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0));
        fprintf(stderr,"API29_POLICY_RAW_SOCKET fd=%d errno=%d\n",raw,raw<0?errno:0);
        if(raw>=0)close(raw);
        FILE* status=fopen("/proc/self/status","re");
        if(status){char line[512];while(fgets(line,sizeof(line),status))
            if(!strncmp(line,"Uid:",4) || !strncmp(line,"Gid:",4) || !strncmp(line,"Groups:",7))
                fprintf(stderr,"API29_POLICY_STATUS %s",line);
            fclose(status);}
        return 11;
    }
    bool routed=connect(route,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 && getsockname(route,reinterpret_cast<sockaddr*>(&address),&length)==0;
    close(route);if(!routed)return 12;
    int receiver=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);if(receiver<0)return 13;
    address.sin_port=0;
    if(bind(receiver,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || getsockname(receiver,reinterpret_cast<sockaddr*>(&address),&length))return 14;
    char ip[INET_ADDRSTRLEN],port[16];if(!inet_ntop(AF_INET,&address.sin_addr,ip,sizeof(ip)))return 15;
    snprintf(port,sizeof(port),"%u",ntohs(address.sin_port));
    auto child=[&](const char* mode){
        pid_t pid=fork();if(pid<0)return false;
        if(!pid){if(setgid(99998) || setuid(99998))_exit(64);execl(executable,executable,mode,ip,port,nullptr);_exit(65);}
        int status=0;return waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status);
    };
    bool blocked=replace(true) && enable(true) && child("policy-denied");
    char packet=0;blocked=blocked && recv(receiver,&packet,1,MSG_DONTWAIT)<0 && (errno==EAGAIN || errno==EWOULDBLOCK);
    bool restored=enable(false) && replace(false);
    bool delivered=restored && child("policy-allowed");
    timeval timeout{2,0};setsockopt(receiver,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    delivered=delivered && recv(receiver,&packet,1,0)==1 && packet=='p';
    if(!blocked || !restored || !delivered){close(receiver);return 16;}
    bool internetDenied=internet(0) && child("policy-denied");
    internetDenied=internetDenied && recv(receiver,&packet,1,MSG_DONTWAIT)<0 && (errno==EAGAIN || errno==EWOULDBLOCK);
    bool internetRestored=internet(4) && child("policy-allowed");
    internetRestored=internetRestored && recv(receiver,&packet,1,0)==1 && packet=='p';
    bool internetCleared=internet(-1);
    close(receiver);
    if(!internetDenied || !internetRestored || !internetCleared)return 17;
    puts("API29_MANAGED_FIREWALL_PASS guest_uid=99998 blocked_before_delivery=1 delivered_after_disable=1 host_kernel_rules=unchanged");return 0;
}
int verifyNetwork() {
    auto manager=defaultServiceManager();if(!manager)return 1;
    for(const char* name:{"netd","dnsresolver"}){
        auto service=manager->getService(String16(name));if(!service)return 2;
        const char* descriptor=!strcmp(name,"netd")?"android.net.INetd":"android.net.IDnsResolver";
        for(uint32_t code:{1u,16777215u}){
            Parcel request,reply;request.writeInterfaceToken(String16(descriptor));
            if(service->transact(code,request,&reply)!=NO_ERROR || reply.readInt32()!=0 ||
                reply.readInt32()!=(code==1?1:2) || reply.dataAvail())return 3;
        }
    }
    auto resolver=manager->getService(String16("dnsresolver"));
    Parcel request,reply;request.writeInterfaceToken(String16("android.net.IDnsResolver"));request.writeInt32(0);
    binder::Status status;
    if(resolver->transact(IDnsResolver_setResolverConfiguration,request,&reply)!=NO_ERROR ||
        status.readFromParcel(reply)!=NO_ERROR || status.serviceSpecificErrorCode()!=ENOTSUP)return 4;
    for(int kind=0;kind<2;kind++){
        int fd=socket(AF_UNIX,(kind?SOCK_STREAM:SOCK_SEQPACKET)|SOCK_CLOEXEC,0);if(fd<0)return 5;
        timeval timeout{3,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        sockaddr_un address{};address.sun_family=AF_UNIX;
        strcpy(address.sun_path,kind?"/dev/socket/fwmarkd":"/dev/socket/lmkd");
        bool ok=connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0;
        if(!kind){
            uint32_t query[3]={htonl(4),htonl(static_cast<uint32_t>(-1000)),htonl(1000)},answer[2]={};
            ok=ok && send(fd,query,sizeof(query),MSG_NOSIGNAL)==sizeof(query) &&
                recv(fd,answer,sizeof(answer),0)==sizeof(answer) && ntohl(answer[0])==4 && answer[1]==0;
        }else{
            uint32_t query[4]={3,0,0,0};int answer=0;
            ok=ok && send(fd,query,sizeof(query),MSG_NOSIGNAL)==sizeof(query) &&
                transfer(fd,&answer,sizeof(answer),false) && answer==-ENOTSUP;
        }
        close(fd);if(!ok)return 6;
    }
    puts("API29_NETWORK_PROTOCOL_PASS binder_version=2 resolver_customization=unsupported lmk_hint_kills=0 vpn_bypass=rejected");return 0;
}
#endif
}
int main(int argc,char** argv) {
#if POCKET_GUEST_API == 29
    if(argc==4 && (!strcmp(argv[1],"policy-denied") || !strcmp(argv[1],"policy-allowed")))return policyPacket(argc,argv);
    if(argc==2 && !strcmp(argv[1],"verify")){
        int result=verifyNetwork();return result?result:verifyPolicy(defaultServiceManager(),argv[0]);
    }
#endif
    (void)argv;if(argc!=1)return 64;
    setbuf(stdout,nullptr);if(getuid()!=0)return 64;
#if POCKET_GUEST_API == 29
    networkPolicy={};networkPolicy.header.magic=POCKET_NP_MAGIC;networkPolicy.header.version=2;
    if(publishPolicy(networkPolicy))return 4;
#endif
    int server=controlSocket("netd"),mdns=controlSocket("mdns"),lmkd=controlSocket("lmkd",SOCK_SEQPACKET);
    int dns=controlSocket("dnsproxyd"),mark=controlSocket("fwmarkd");
    if(server<0 || mdns<0 || lmkd<0 || dns<0 || mark<0) {perror("private control socket");return 1;}
    auto manager=defaultServiceManager();if(!manager || manager->addService(String16("netd"),new HostNetd)!=NO_ERROR)return 2;
#if POCKET_GUEST_API == 29
    if(manager->addService(String16("dnsresolver"),new HostResolver)!=NO_ERROR)return 3;
#endif
    ProcessState::self()->startThreadPool();std::thread(acceptLoop,server,false).detach();std::thread(acceptLoop,mdns,false).detach();std::thread(acceptLoop,lmkd,true).detach();
    std::thread(relayLoop,dns,true).detach();std::thread(relayLoop,mark,false).detach();
    puts("DIRECT_NETWORK_READY profile=host_delegated host_routes_firewall_marks=unchanged");
    IPCThreadState::self()->joinThreadPool();return 0;
}
