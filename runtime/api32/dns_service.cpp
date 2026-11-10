#include "service_output.h"
// SPDX-License-Identifier: GPL-3.0-only
// Android 12L resolver control and dnsproxyd, using the real outer app resolver.
#include <android/net/BnDnsResolver.h>
#include <binder/IServiceManager.h>
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <sys/system_properties.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include "host_dns.h"
#include "dns_packet.h"
#include "binder_listeners.h"
#include "kernel_identity.h"
#include "../api28/binder_identity.h"
#include "../loader/network_policy.h"
using namespace android;
using namespace android::net;
using binder::Status;
namespace {
// Pinned DnsResolver gethnamaddr.h: NETD_RESOLV_TIMEOUT, also RCODE_TIMEOUT.
constexpr int dnsTimeoutEvent32=255;
Status error(int value,const char* message) {return Status::fromServiceSpecificError(value,message);}
Status unsupported() {return error(EOPNOTSUPP,"DNS customization is owned by the host Android resolver");}
bool validNetId(int netId) {return netId>0 && netId<=65535;}
bool policyAllows(unsigned uid) {
    if(uid>=POCKET_NP_UIDS)return false;
    int fd=open(POCKET_NP_PATH,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return false;
    PocketNetworkPolicyV3 policy{};struct stat info{};
    bool ok=!fstat(fd,&info) && S_ISREG(info.st_mode) && info.st_size==sizeof(policy);
    size_t count=0;
    while(ok && count<sizeof(policy)) {
        ssize_t readCount=read(fd,reinterpret_cast<char*>(&policy)+count,sizeof(policy)-count);
        if(readCount<0 && errno==EINTR)continue;
        if(readCount<=0){ok=false;break;}count+=readCount;
    }
    close(fd);
    if(!ok || count!=sizeof(policy) || !pocket_np_v3_valid(&policy.header))return false;
    for(unsigned chain=0;chain<POCKET_NP_CHAINS_V3;++chain)
        if((policy.header.enabled&(1U<<chain)) && pocket_np_member_denies(chain,uid,
            !!(policy.members[chain][uid/8]&(1U<<(uid%8)))))return false;
    return true;
}
class Resolver32 final : public BnDnsResolver {
    std::mutex mutex;
    std::map<int,ResolverParamsParcel> networks;
    sp<BinderListeners32<metrics::INetdEventListener>> events=new BinderListeners32<metrics::INetdEventListener>;
    sp<BinderListeners32<resolv::aidl::IDnsResolverUnsolicitedEventListener>> unsolicited=new BinderListeners32<resolv::aidl::IDnsResolverUnsolicitedEventListener>;
    std::atomic<int> severity{DNS_RESOLVER_LOG_INFO};
public:
    bool known(unsigned netId) {
        if(!netId)return true;
        std::lock_guard<std::mutex> lock(mutex);return networks.count(netId);
    }
    void report(unsigned netId,unsigned uid,const std::string& hostname,int result,int milliseconds,
                const std::vector<std::vector<uint8_t>>& addresses,int event=metrics::INetdEventListener::EVENT_GETADDRINFO) {
        auto text=dnsStrings32(addresses);
        for(const auto& listener:events->snapshot())
            listener->onDnsEvent(netId,event,result,milliseconds,hostname,text,text.size(),uid);
        if(severity.load()<=DNS_RESOLVER_LOG_INFO)
            fprintf(stderr,"API32_DNS_QUERY uid=%u netid=%u result=%d addresses=%zu host_resolver=1\n",uid,netId,result,addresses.size());
    }
    status_t onTransact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags) override {
        if(code>=IBinder::FIRST_CALL_TRANSACTION && code<=IBinder::LAST_CALL_TRANSACTION) {
            uid_t uid=IPCThreadState::self()->getCallingUid();
            if(uid!=0 && uid!=1000 && uid!=1073) {
                if(!reply)return PERMISSION_DENIED;
                return Status::fromExceptionCode(Status::EX_SECURITY,"Guest resolver requires a system role").writeToParcel(reply);
            }
        }
        return BnDnsResolver::onTransact(code,data,reply,flags);
    }
    Status isAlive(bool* value) override {*value=true;return Status::ok();}
    Status registerEventListener(const sp<metrics::INetdEventListener>& listener) override {
        auto result=events->add(listener);
        if(result.isOk())puts("API32_DNS_EVENT_LISTENER_REGISTERED typed=1");return result;
    }
    Status registerUnsolicitedEventListener(const sp<resolv::aidl::IDnsResolverUnsolicitedEventListener>& listener) override {
        auto result=unsolicited->add(listener);
        if(result.isOk())puts("API32_DNS_UNSOLICITED_LISTENER_REGISTERED typed=1");return result;
    }
    Status createNetworkCache(int32_t netId) override {
        if(!validNetId(netId))return error(EINVAL,"Invalid guest network ID");
        std::lock_guard<std::mutex> lock(mutex);
        if(networks.count(netId))return error(EEXIST,"Resolver network already exists");
        ResolverParamsParcel config;config.netId=netId;networks.emplace(netId,config);return Status::ok();
    }
    Status destroyNetworkCache(int32_t netId) override {
        std::lock_guard<std::mutex> lock(mutex);
        return networks.erase(netId)?Status::ok():error(ENONET,"Unknown resolver network");
    }
    Status flushNetworkCache(int32_t netId) override {
        // This profile retains no guest answer cache. Do not flush the shared
        // host resolver cache used by other host applications.
        return known(netId) && netId?Status::ok():error(ENONET,"Unknown resolver network");
    }
    Status setResolverConfiguration(const ResolverParamsParcel& config) override {
        if(!validNetId(config.netId) || config.sampleValiditySeconds<0 || config.successThreshold<0 || config.successThreshold>100 ||
           config.minSamples<0 || config.maxSamples<config.minSamples || config.baseTimeoutMsec<0 || config.retryCount<0)
            return error(EINVAL,"Invalid resolver parameters");
        if(!config.domains.empty() || !config.tlsName.empty() || !config.tlsServers.empty() || !config.tlsFingerprints.empty() ||
           !config.caCertificate.empty() || config.tlsConnectTimeoutMs || !config.resolverOptions.hosts.empty() ||
           config.resolverOptions.tcMode || config.resolverOptions.enforceDnsUid)return unsupported();
        if(!config.servers.empty()) {
            std::vector<std::vector<uint8_t>> current;
            if(!hostDns32(16,"",current))return error(ENETDOWN,"Host DNS configuration unavailable");
            auto servers=dnsStrings32(current);
            if(config.servers!=servers)return unsupported();
        }
        std::lock_guard<std::mutex> lock(mutex);auto found=networks.find(config.netId);
        if(found==networks.end())return error(ENONET,"Unknown resolver network");
        found->second=config;return Status::ok();
    }
    Status getResolverInfo(int32_t netId,std::vector<std::string>* servers,std::vector<std::string>* domains,
            std::vector<std::string>* tlsServers,std::vector<int32_t>* params,std::vector<int32_t>* stats,
            std::vector<int32_t>* waiting) override {
        std::lock_guard<std::mutex> lock(mutex);auto found=networks.find(netId);
        if(found==networks.end())return error(ENONET,"Unknown resolver network");
        const auto& config=found->second;*servers=config.servers;*domains=config.domains;*tlsServers=config.tlsServers;
        *params={config.sampleValiditySeconds,config.successThreshold,config.minSamples,config.maxSamples,config.baseTimeoutMsec,config.retryCount};
        // No per-nameserver queries or cache-coalescing waits are performed in
        // the guest. Host query statistics are not available through this API.
        stats->assign(servers->size()*RESOLVER_STATS_COUNT,0);*waiting={0};return Status::ok();
    }
    Status startPrefix64Discovery(int32_t) override {return unsupported();}
    Status stopPrefix64Discovery(int32_t netId) override {return known(netId)?Status::ok():error(ENONET,"Unknown resolver network");}
    Status getPrefix64(int32_t netId,std::string* prefix) override {
        if(!known(netId))return error(ENONET,"Unknown resolver network");
        prefix->clear();return Status::ok();
    }
    Status setPrefix64(int32_t netId,const std::string& prefix) override {
        if(!known(netId))return error(ENONET,"Unknown resolver network");
        return prefix.empty()?Status::ok():unsupported();
    }
    Status setLogSeverity(int32_t value) override {
        if(value<DNS_RESOLVER_LOG_VERBOSE || value>DNS_RESOLVER_LOG_ERROR)return error(EINVAL,"Invalid DNS log severity");
        severity.store(value);return Status::ok();
    }
};
void failure(int fd,int error) {
    char response[]="401";dnsTransfer32(fd,response,sizeof(response),true);dnsTransfer32(fd,&error,sizeof(error),true);
}
void query(int fd,const sp<Resolver32>& resolver) {
    ucred peer{};socklen_t length=sizeof(peer);uint64_t identity=0;
    if(getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&length) || peer.uid!=pocket_kernel_uid_from_proc() ||
       !pocket_binder_identity(peer.pid,&identity)){failure(fd,EAI_FAIL);return;}
    std::string request;char byte=0;
    while(request.size()<11000) {
        ssize_t count=recv(fd,&byte,1,0);if(count<0 && errno==EINTR)continue;
        if(count!=1)return;if(!byte)break;request+=byte;
    }
    if(byte || request.size()>=11000){if(request.rfind("resnsend ",0)==0)dnsWord32(fd,-EMSGSIZE);else failure(fd,EAI_FAIL);return;}
    std::string operation,hostname,service,extra;int flags,family,type,protocol;unsigned netId;
    std::istringstream input(request);
    if(!(input>>operation)){failure(fd,EAI_FAIL);return;}
    if(operation=="resnsend") {
        std::string encoded,netText,flagText;unsigned rawFlags=0;
        if(!(input>>netText>>flagText>>encoded) || input>>extra || !dnsUnsigned32(netText,netId)
                || !dnsUnsigned32(flagText,rawFlags) || (rawFlags&~7U)) {dnsWord32(fd,-EINVAL);return;}
        bool bypass=(netId&0x80000000U)!=0;netId&=0x7fffffffU;unsigned uid=identity>>32;
        if(!resolver->known(netId)){dnsWord32(fd,-ENONET);return;}
        if(!policyAllows(uid) || (bypass && uid!=0 && uid!=1000 && uid!=1073)) {
            puts("API32_DNS_PACKET_PERMISSION_DENIED host_query=not_sent");dnsWord32(fd,-EACCES);return;
        }
        std::vector<uint8_t> packet,answer;std::string name;unsigned rrtype=0;
        if(!dnsDecode6432(encoded,packet)){dnsWord32(fd,-EILSEQ);return;}
        if(!dnsQuestion32(packet,name,&rrtype)){dnsWord32(fd,-EINVAL);return;}
        auto started=std::chrono::steady_clock::now();
        unsigned rcode=0;int status=hostDnsPacket32(rawFlags,bypass,packet,rcode,answer);
        int elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
        if(!status && (answer.size()<12 || !(answer[2]&0x80) || answer[0]!=packet[0] || answer[1]!=packet[1]))status=-EBADMSG;
        if(rrtype==1 || rrtype==28)resolver->report(netId,uid,name,status?(status==-ETIMEDOUT?dnsTimeoutEvent32:EAI_SYSTEM):(rcode?EAI_NODATA:0),
            elapsed,status?std::vector<std::vector<uint8_t>>{}:dnsAddresses32(answer,rrtype),metrics::INetdEventListener::EVENT_RES_NSEND);
        if(status){dnsWord32(fd,status);return;}
        dnsWord32(fd,rcode);dnsBlock32(fd,answer.data(),answer.size());
        fprintf(stderr,"API32_DNS_PACKET_QUERY uid=%u netid=%u rcode=%u bytes=%zu bypass=%u\n",uid,netId,rcode,answer.size(),bypass);
        return;
    }
    if(operation=="getdnsnetid") {
        int result=-EINVAL;std::string text;
        if((input>>text) && !(input>>extra) && dnsUnsigned32(text,netId)) {
            unsigned plain=netId&0x7fffffffU,uid=identity>>32;bool bypass=netId!=plain;
            if(resolver->known(plain))result=policyAllows(uid) && (!bypass || uid==0 || uid==1000 || uid==1073)?static_cast<int>(netId):-EACCES;
        }
        char response[]="222";dnsTransfer32(fd,response,sizeof(response),true);dnsWord32(fd,result);return;
    }
    if(!(input>>hostname>>service>>flags>>family>>type>>protocol>>netId) || input>>extra || operation!="getaddrinfo") {
        failure(fd,EAI_FAIL);return;
    }
    if(flags==-1)flags=0;if(family==-1)family=AF_UNSPEC;if(type==-1)type=0;if(protocol==-1)protocol=0;
    if(flags&~(AI_PASSIVE|AI_NUMERICHOST|AI_NUMERICSERV|AI_ADDRCONFIG|AI_V4MAPPED|AI_ALL)){failure(fd,EAI_BADFLAGS);return;}
    if(family!=AF_UNSPEC && family!=AF_INET && family!=AF_INET6){failure(fd,EAI_FAMILY);return;}
    if((type!=0 && type!=SOCK_STREAM && type!=SOCK_DGRAM) ||
       (protocol!=0 && protocol!=IPPROTO_TCP && protocol!=IPPROTO_UDP) ||
       (type==SOCK_STREAM && protocol==IPPROTO_UDP) || (type==SOCK_DGRAM && protocol==IPPROTO_TCP)) {failure(fd,EAI_SOCKTYPE);return;}
    unsigned uid=identity>>32;
    bool bypass=(netId&0x80000000U)!=0;netId&=0x7fffffffU;
    if(!resolver->known(netId)){failure(fd,EAI_FAIL);return;}
    if(bypass && uid!=0 && uid!=1000 && uid!=1073) {
        puts("API32_DNS_BYPASS_PERMISSION_DENIED host_query=not_sent");failure(fd,EAI_FAIL);return;
    }
    // Local/numeric lookups do not leak a query to the outer DNS resolver.
    uint8_t numeric[16]{};int numericFamily=inet_pton(AF_INET,hostname.c_str(),numeric)==1?AF_INET:
        inet_pton(AF_INET6,hostname.c_str(),numeric)==1?AF_INET6:0;
    if((flags&AI_NUMERICHOST) && !numericFamily){failure(fd,EAI_NONAME);return;}
    if(hostname!="^" && hostname!="localhost" && !numericFamily && !policyAllows(uid)) {
        puts("API32_DNS_PERMISSION_DENIED host_query=not_sent");failure(fd,EAI_FAIL);return;
    }
    unsigned port=0;
    if(service!="^") {
        char* end=nullptr;errno=0;unsigned long value=strtoul(service.c_str(),&end,10);
        if(errno || !end || *end || value>65535) {
            if(flags&AI_NUMERICSERV){failure(fd,EAI_NONAME);return;}
            auto* entry=getservbyname(service.c_str(),type==SOCK_DGRAM || protocol==IPPROTO_UDP?"udp":"tcp");
            if(!entry){failure(fd,EAI_SERVICE);return;}port=ntohs(entry->s_port);
        }else port=value;
    }
    auto started=std::chrono::steady_clock::now();std::vector<std::vector<uint8_t>> addresses;
    if(numericFamily)addresses.emplace_back(numeric,numeric+(numericFamily==AF_INET?4:16));
    else if(hostname=="^" || hostname=="localhost") {
        addresses.push_back({0,0,0,0});addresses.push_back(std::vector<uint8_t>(16));
        if(hostname=="localhost" || !(flags&AI_PASSIVE)){addresses[0]={127,0,0,1};addresses[1][15]=1;}
    }else if(!(bypass?hostDnsBypass32(hostname,family,flags&AI_V4MAPPED,addresses):hostDns32(2,hostname,addresses)) || addresses.empty()) {
        resolver->report(netId,uid,hostname,EAI_NONAME,0,{});failure(fd,EAI_NONAME);return;
    }
    auto original=addresses;
    bool hasV6=std::any_of(addresses.begin(),addresses.end(),[](const auto& address){return address.size()==16;});
    if(family==AF_INET6 && (flags&AI_V4MAPPED) && (!hasV6 || (flags&AI_ALL))) {
        for(const auto& address:original)if(address.size()==4) {
            std::vector<uint8_t> mapped(16);mapped[10]=255;mapped[11]=255;
            std::copy(address.begin(),address.end(),mapped.begin()+12);addresses.push_back(mapped);
        }
    }
    unsigned records=0;for(const auto& address:addresses)if(family==AF_UNSPEC || family==(address.size()==4?AF_INET:AF_INET6))++records;
    if(!records){failure(fd,EAI_NONAME);return;}
    char response[]="222";if(!dnsTransfer32(fd,response,sizeof(response),true))return;
    for(const auto& address:addresses) {
        int af=address.size()==4?AF_INET:AF_INET6;if(family!=AF_UNSPEC && family!=af)continue;
        sockaddr_storage storage{};socklen_t size;
        if(af==AF_INET) {
            auto* value=reinterpret_cast<sockaddr_in*>(&storage);value->sin_family=AF_INET;value->sin_port=htons(port);
            memcpy(&value->sin_addr,address.data(),4);size=sizeof(*value);
        }else {
            auto* value=reinterpret_cast<sockaddr_in6*>(&storage);value->sin6_family=AF_INET6;value->sin6_port=htons(port);
            memcpy(&value->sin6_addr,address.data(),16);size=sizeof(*value);
        }
        for(int socketType:{SOCK_STREAM,SOCK_DGRAM}) {
            int transport=socketType==SOCK_STREAM?IPPROTO_TCP:IPPROTO_UDP;
            if((type && type!=socketType) || (protocol && protocol!=transport))continue;
            if(!dnsWord32(fd,1) || !dnsWord32(fd,flags) || !dnsWord32(fd,af) || !dnsWord32(fd,socketType) ||
               !dnsWord32(fd,transport) || !dnsBlock32(fd,&storage,size) || !dnsWord32(fd,0))return;
        }
    }
    dnsWord32(fd,0);
    auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
    resolver->report(netId,uid,hostname,0,elapsed,original);
}
}
int main() {
    pocketServiceOutput32();char sdk[PROP_VALUE_MAX];
    if(getuid()!=0 || syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL ||
       __system_property_get("ro.build.version.sdk",sdk)<=0 || strcmp(sdk,"32"))return 64;
    int server=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(server<0)return 1;
    sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/dnsproxyd");unlink(address.sun_path);
    if(bind(server,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || chmod(address.sun_path,0666) || listen(server,16))return 2;
    sp<Resolver32> resolver=new Resolver32;
    ProcessState::self()->startThreadPool();auto manager=defaultServiceManager();
    if(!manager || manager->addService(String16("dnsresolver"),resolver)!=NO_ERROR)return 3;
    puts("API32_DNS_READY typed_aidl=7 host_resolver=1 guest_answer_cache=none");
    std::atomic<unsigned> clients{0};
    for(;;) {
        int fd=accept4(server,nullptr,nullptr,SOCK_CLOEXEC);if(fd<0){if(errno==EINTR)continue;return 4;}
        if(clients.fetch_add(1)>=32){clients.fetch_sub(1);close(fd);continue;}
        std::thread([fd,resolver,&clients]{
            timeval timeout{15,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
            setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
            query(fd,resolver);close(fd);clients.fetch_sub(1);
        }).detach();
    }
}
