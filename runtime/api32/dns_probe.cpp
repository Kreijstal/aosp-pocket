// SPDX-License-Identifier: GPL-3.0-only
// Stock typed resolver clients, real Bionic lookup and host-backed DNS results.
#include <android/net/IDnsResolver.h>
#include <android/net/INetd.h>
#include <android/net/metrics/BnNetdEventListener.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <netdb.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <atomic>
#include <algorithm>
#include <sstream>
#include "host_dns.h"
#include "dns_packet.h"
using namespace android;
using namespace android::net;
using binder::Status;
namespace {
bool ok(const Status& status,const char* operation) {
    if(status.isOk())return true;
    fprintf(stderr,"API32_DNS_ERROR operation=%s status=%s\n",operation,status.toString8().c_str());return false;
}
bool specific(const Status& status,int error) {
    return status.exceptionCode()==Status::EX_SERVICE_SPECIFIC && status.serviceSpecificErrorCode()==error;
}
template<class Interface> sp<Interface> service(const char* name) {
    ProcessState::self()->startThreadPool();auto manager=defaultServiceManager();if(!manager)return nullptr;
    for(unsigned n=0;n<200;++n) {
        auto binder=manager->checkService(String16(name));if(binder)return interface_cast<Interface>(binder);
        usleep(50000);
    }
    return nullptr;
}
class Events final : public metrics::BnNetdEventListener {
public:
    std::atomic<unsigned> successful{0};
    std::atomic<unsigned> rawSuccessful{0};
    Status onDnsEvent(int32_t netId,int32_t event,int32_t code,int32_t latency,const std::string& hostname,
            const std::vector<std::string>& addresses,int32_t count,int32_t uid) override {
        if(netId==0 && event==EVENT_GETADDRINFO && !code && latency>=0 && hostname=="example.com" &&
           !addresses.empty() && count==static_cast<int>(addresses.size()) && uid==99998)++successful;
        if(netId==0 && event==EVENT_RES_NSEND && !code && latency>=0 && hostname=="example.com" &&
           !addresses.empty() && count==static_cast<int>(addresses.size()) && uid==99998)++rawSuccessful;
        return Status::ok();
    }
    Status onPrivateDnsValidationEvent(int32_t,const String16&,const String16&,bool) override {return Status::ok();}
    Status onConnectEvent(int32_t,int32_t,int32_t,const String16&,int32_t,int32_t) override {return Status::ok();}
    Status onWakeupEvent(const String16&,int32_t,int32_t,int32_t,const std::vector<uint8_t>&,
            const String16&,const String16&,int32_t,int32_t,int64_t) override {return Status::ok();}
    Status onTcpSocketStatsEvent(const std::vector<int32_t>&,const std::vector<int32_t>&,const std::vector<int32_t>&,
            const std::vector<int32_t>&,const std::vector<int32_t>&) override {return Status::ok();}
    Status onNat64PrefixEvent(int32_t,bool,const std::string&,int32_t) override {return Status::ok();}
};
int rawCommand(const std::string& command,std::vector<uint8_t>& answer,bool legacyFailure=false) {
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -errno;
    timeval timeout{12,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/dnsproxyd");
    uint32_t code=0,size=0;int result=-EIO;
    if(!connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)) &&
       dnsTransfer32(fd,const_cast<char*>(command.c_str()),command.size()+1,true) && dnsTransfer32(fd,&code,4,false)) {
        if(legacyFailure) {
            int error=0;
            result=!memcmp(&code,"401",4) && dnsTransfer32(fd,&error,4,false)?error:-EBADMSG;
            close(fd);return result;
        }
        result=static_cast<int32_t>(ntohl(code));
        if(result>=0) {
            if(!dnsTransfer32(fd,&size,4,false) || (size=ntohl(size))<12 || size>8192)result=-EBADMSG;
            else {answer.resize(size);if(!dnsTransfer32(fd,answer.data(),size,false))result=-EIO;}
        }
    }
    close(fd);return result;
}
int rawLookup(int argc,char** argv) {
    if(argc!=3 || getuid()!=99998)return 64;
    bool denied=!strcmp(argv[1],"raw-lookup-denied");
    constexpr const char* encoded="SjkBAAABAAAAAAAAB2V4YW1wbGUDY29tAAABAAE=";
    std::vector<uint8_t> query,answer;unsigned rcode=0;dnsDecode6432(encoded,query);
    if(hostDnsPacket32(6,false,query,rcode,answer)!=-EACCES)return 65;
    puts("API32_DNS_PACKET_HOST_DIRECT_DENIED_PASS uid=99998");
    std::string command=std::string("resnsend 0 6 ")+encoded;
    int status=rawCommand(command,answer);
    if(denied) {
        if(status!=-EACCES)return 66;
        puts("API32_DNS_PACKET_PERMISSION_DENIED_PASS uid=99998 host_query=blocked");return 0;
    }
    if(status || answer.size()<12 || answer[0]!=query[0] || answer[1]!=query[1] || dnsAddresses32(answer,1).empty())return 67;
    if(rawCommand(std::string("resnsend 2147483648 6 ")+encoded,answer)!=-EACCES)return 68;
    if(rawCommand("getaddrinfo example.com 443 0 2 1 6 2147483648",answer,true)!=EAI_FAIL)return 70;
    if(rawCommand("resnsend 0 6 !!!!",answer)!=-EILSEQ || rawCommand("resnsend 0 6 Zg==",answer)!=-EINVAL ||
       rawCommand(std::string("resnsend 0 8 ")+encoded,answer)!=-EINVAL ||
       rawCommand(std::string("resnsend -1 6 ")+encoded,answer)!=-EINVAL ||
       rawCommand(std::string("resnsend 64512 6 ")+encoded,answer)!=-ENONET)return 69;
    puts("API32_DNS_PACKET_LOOKUP_PASS uid=99998 original_id=restored answers=real bypass=denied invalid=denied");return 0;
}
int lookup(int argc,char** argv) {
    if(argc!=3 || getuid()!=99998)return 64;
    bool denied=!strcmp(argv[1],"lookup-denied");
    std::vector<std::vector<uint8_t>> direct;
    if(hostDns32(2,"example.com",direct))return 65;
    puts("API32_DNS_HOST_DIRECT_DENIED_PASS uid=99998");
    addrinfo hints{};hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;addrinfo* result=nullptr;
    int status=getaddrinfo("example.com","443",&hints,&result);
    if(denied) {
        if(!status){freeaddrinfo(result);return 66;}
        puts("API32_DNS_PERMISSION_DENIED_PASS uid=99998 host_query=blocked");return 0;
    }
    if(status){fprintf(stderr,"API32_DNS_LOOKUP_FAILED error=%d detail=%s\n",status,gai_strerror(status));return 67;}
    std::vector<std::string> expected;std::istringstream input(argv[2]);std::string address;
    while(std::getline(input,address,','))expected.push_back(address);
    unsigned matches=0;bool connected=false;
    for(auto* item=result;item;item=item->ai_next) {
        if(item->ai_family!=AF_INET || item->ai_socktype!=SOCK_STREAM || item->ai_protocol!=IPPROTO_TCP)continue;
        char text[INET_ADDRSTRLEN];auto* ip=reinterpret_cast<sockaddr_in*>(item->ai_addr);
        if(!inet_ntop(AF_INET,&ip->sin_addr,text,sizeof(text)) || ntohs(ip->sin_port)!=443)continue;
        if(std::find(expected.begin(),expected.end(),text)==expected.end())continue;
        ++matches;if(connected)continue;
        int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)continue;
        int value=connect(fd,item->ai_addr,item->ai_addrlen);
        if(value==0)connected=true;
        else if(errno==EINPROGRESS) {
            pollfd ready{fd,POLLOUT,0};int answer;
            do{answer=poll(&ready,1,8000);}while(answer<0 && errno==EINTR);
            int error=0;socklen_t size=sizeof(error);
            if(answer==1 && !getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size) && !error)connected=true;
        }
        close(fd);
    }
    freeaddrinfo(result);
    if(!matches || !connected)return 68;
    printf("API32_DNS_LOOKUP_PASS uid=99998 libc_getaddrinfo=1 host_answer_matches=%u external_tcp=connected\n",matches);
    return 0;
}
bool child(const char* executable,const char* mode,const std::string& expected,int uid) {
    pid_t pid=fork();if(pid<0)return false;
    if(!pid) {
        if(setgid(uid) || setuid(uid))_exit(64);
        execl(executable,executable,mode,expected.c_str(),nullptr);_exit(65);
    }
    int status=0;
    while(waitpid(pid,&status,0)<0)if(errno!=EINTR)return false;
    if(!WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr,"API32_DNS_CHILD_FAILED mode=%s status=%d\n",mode,status);return false;
    }
    return true;
}
}
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);
    if(argc>=2 && !strncmp(argv[1],"raw-lookup",10))return rawLookup(argc,argv);
    if(argc>=2 && !strncmp(argv[1],"lookup",6))return lookup(argc,argv);
    bool exiting=argc==3 && !strcmp(argv[1],"listener-exit");
    if(exiting) {
        if(getuid()!=1000)return 64;auto target=service<IDnsResolver>("dnsresolver");
        return target && target->registerEventListener(new Events).isOk()?0:69;
    }
    if(argc!=2 || getuid()!=0)return 64;
    bool denied=!strcmp(argv[1],"denied"),check=!strcmp(argv[1],"check"),resolve=!strcmp(argv[1],"resolve"),raw=!strcmp(argv[1],"raw");
    if(!denied && !check && !resolve && !raw)return 64;
    if(!resolve && !raw){int uid=denied?39999:1000;if(setgid(uid) || setuid(uid))return 64;}
    auto target=service<IDnsResolver>("dnsresolver");if(!target)return 1;
    bool alive=false;auto status=target->isAlive(&alive);
    if(denied) {
        if(status.exceptionCode()!=Status::EX_SECURITY)return 2;
        puts("API32_DNS_ROLE_DENIED_PASS uid=39999 operation=isAlive");return 0;
    }
    if(!ok(status,"isAlive") || !alive || target->getInterfaceVersion()!=7 ||
        target->getInterfaceHash()!="a1dc9394598357ccaa74e96f564e7f248b72bad2")return 3;
    if(raw) {
        auto netd=service<INetd>("netd");if(!netd)return 20;
        sp<Events> events=new Events;if(!ok(target->registerEventListener(events),"rawEventListener"))return 21;
        bool passed=ok(netd->trafficSetNetPermForUids(INetd::PERMISSION_INTERNET,{99998}),"grantRawInternet")
            && child(argv[0],"raw-lookup","",99998)
            && ok(netd->trafficSetNetPermForUids(INetd::NO_PERMISSIONS,{99998}),"denyRawInternet")
            && child(argv[0],"raw-lookup-denied","",99998)
            && ok(netd->trafficSetNetPermForUids(INetd::PERMISSION_INTERNET,{99998}),"restoreRawInternet")
            && child(argv[0],"raw-lookup","",99998);
        bool cleaned=ok(netd->trafficSetNetPermForUids(INetd::NO_PERMISSIONS,{99998}),"cleanupRawInternet");
        for(unsigned n=0;n<100 && events->rawSuccessful.load()<2;++n)usleep(20000);
        if(!passed || !cleaned || events->rawSuccessful.load()!=2)return 22;
        puts("API32_DNS_PACKET_RESOLVE_PASS uid_permission=grant_revoke_restore private_dns_bypass=denied typed_events=2 cleanup=revoked");return 0;
    }
    if(resolve) {
        auto netd=service<INetd>("netd");if(!netd)return 10;
        sp<Events> events=new Events;
        if(!ok(target->registerEventListener(events),"registerEventListener"))return 11;
        std::vector<std::vector<uint8_t>> addresses;
        if(!hostDns32(2,"example.com",addresses) || addresses.empty())return 12;
        std::string expected;for(const auto& address:dnsStrings32(addresses)){if(!expected.empty())expected+=',';expected+=address;}
        if(!ok(netd->trafficSetNetPermForUids(INetd::PERMISSION_INTERNET,{99998}),"grantInternet") ||
           !child(argv[0],"lookup",expected,99998))return 13;
        if(!ok(netd->trafficSetNetPermForUids(INetd::NO_PERMISSIONS,{99998}),"denyInternet") ||
           !child(argv[0],"lookup-denied",expected,99998))return 14;
        if(!ok(netd->trafficSetNetPermForUids(INetd::PERMISSION_INTERNET,{99998}),"restoreInternet") ||
           !child(argv[0],"lookup",expected,99998))return 15;
        if(!ok(netd->trafficSetNetPermForUids(INetd::NO_PERMISSIONS,{99998}),"cleanupInternet"))return 16;
        for(unsigned n=0;n<100 && events->successful.load()<2;++n)usleep(20000);
        if(events->successful.load()<2)return 17;
        for(unsigned n=0;n<40;++n) {
            if(!child(argv[0],"listener-exit","",1000))return 18;
            usleep(10000);
        }
        puts("API32_DNS_LISTENER_DEATH_PASS exited_clients=40 registrations_reclaimed=1");
        puts("API32_DNS_RESOLVE_PASS libc_getaddrinfo=1 host_answers=verified external_tcp=1 uid_permission_enforced=1 typed_events=2");return 0;
    }
    constexpr int netId=64513;
    if(!ok(target->createNetworkCache(netId),"createNetworkCache") || !specific(target->createNetworkCache(netId),EEXIST))return 4;
    ResolverParamsParcel config;config.netId=netId;config.sampleValiditySeconds=300;config.successThreshold=25;
    config.minSamples=8;config.maxSamples=64;config.baseTimeoutMsec=5000;config.retryCount=2;
    std::vector<std::vector<uint8_t>> host;
    if(!hostDns32(16,"",host) || host.empty())return 5;
    config.servers=dnsStrings32(host);
    if(!ok(target->setResolverConfiguration(config),"setResolverConfiguration"))return 6;
    std::vector<std::string> servers,domains,tls;std::vector<int32_t> params,stats,waiting;
    if(!ok(target->getResolverInfo(netId,&servers,&domains,&tls,&params,&stats,&waiting),"getResolverInfo") ||
       servers!=config.servers || !domains.empty() || !tls.empty() || params!=std::vector<int32_t>({300,25,8,64,5000,2}) ||
       stats.size()!=servers.size()*IDnsResolver::RESOLVER_STATS_COUNT || waiting!=std::vector<int32_t>({0}))return 7;
    config.tlsName="dns.example";
    if(!specific(target->setResolverConfiguration(config),EOPNOTSUPP))return 8;
    config.tlsName.clear();config.servers={"192.0.2.1"};
    if(!specific(target->setResolverConfiguration(config),EOPNOTSUPP) || !ok(target->flushNetworkCache(netId),"flushNetworkCache") ||
       !specific(target->startPrefix64Discovery(netId),EOPNOTSUPP) || !ok(target->destroyNetworkCache(netId),"destroyNetworkCache") ||
       !specific(target->getResolverInfo(netId,&servers,&domains,&tls,&params,&stats,&waiting),ENONET) ||
       !specific(target->createNetworkCache(-1),EINVAL))return 9;
    puts("API32_DNS_CONTROL_PASS typed_aidl=7 hash=verified cache_lifecycle=1 host_servers=matched customization=rejected");return 0;
}
