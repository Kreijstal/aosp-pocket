// SPDX-License-Identifier: GPL-3.0-only
// Exercise stock typed netd proxies and actual interpreted UDP permission rules.
#include <android/net/INetd.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <FwmarkCommand.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
using namespace android;
using namespace android::net;
namespace {
bool ok(const binder::Status& status,const char* operation) {
    if(status.isOk())return true;
    fprintf(stderr,"API32_NETD_ERROR operation=%s status=%s\n",operation,status.toString8().c_str());return false;
}
bool specific(const binder::Status& status,int code) {
    return status.exceptionCode()==binder::Status::EX_SERVICE_SPECIFIC && status.serviceSpecificErrorCode()==code;
}
sp<INetd> service() {
    ProcessState::self()->startThreadPool();auto manager=defaultServiceManager();if(!manager)return nullptr;
    for(unsigned n=0;n<200;++n) {
        auto binder=manager->checkService(String16("netd"));
        if(binder)return interface_cast<INetd>(binder);
        usleep(50000);
    }
    return nullptr;
}
int mark(int socketFd,FwmarkCommand::CmdId operation,unsigned network,bool passFd=true) {
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -errno;
    timeval timeout{2,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/fwmarkd");
    FwmarkCommand command{operation,network,39999,0}; // Claimed UID must not replace SO_PEERCRED identity.
    iovec buffer{&command,sizeof(command)};msghdr message{};
    message.msg_iov=&buffer;message.msg_iovlen=1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    if(passFd) {
        message.msg_control=control;message.msg_controllen=sizeof(control);
        auto* header=CMSG_FIRSTHDR(&message);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;
        header->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(header),&socketFd,sizeof(socketFd));
    }
    int result=-EIO;
    if(!connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)) &&
        sendmsg(fd,&message,MSG_NOSIGNAL)==sizeof(command) && recv(fd,&result,sizeof(result),MSG_WAITALL)==sizeof(result)) {
        close(fd);return result;
    }
    close(fd);return -EIO;
}
bool fwmarkErrors() {
    sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/fwmarkd");
    int idle=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(idle<0)return false;
    timeval timeout{5,0};int result=0;
    bool okay=!setsockopt(idle,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)) &&
        !connect(idle,reinterpret_cast<sockaddr*>(&address),sizeof(address)) &&
        recv(idle,&result,sizeof(result),MSG_WAITALL)==sizeof(result) && result==-EINVAL;
    close(idle);if(!okay)return false;
    int eof=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(eof<0)return false;
    okay=!connect(eof,reinterpret_cast<sockaddr*>(&address),sizeof(address));close(eof);
    if(!okay || mark(-1,FwmarkCommand::SELECT_NETWORK,0,false)!=-EINVAL)return false;
    puts("API32_FWMARK_ERRORS_PASS idle_timeout=1 eof=1 subsequent_request=1 daemon_alive=1");return true;
}
int packet(int argc,char** argv) {
    if(argc!=4 || getuid()!=99998)return 64;
    bool socketDenied=!strcmp(argv[1],"packet-socket-denied");
    bool denied=socketDenied || !strcmp(argv[1],"packet-denied");
    sockaddr_in address{};address.sin_family=AF_INET;char* end=nullptr;long port=strtol(argv[3],&end,10);
    if(!end || *end || port<1 || port>65535 || inet_pton(AF_INET,argv[2],&address.sin_addr)!=1)return 65;
    address.sin_port=htons(port);
    int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);
    if(fd<0) {
        if(socketDenied && errno==EACCES){puts("API32_NETD_PACKET_BLOCK stage=socket");return 0;}
        return 66;
    }
    if(socketDenied){close(fd);return 68;}
    ssize_t sent=sendto(fd,"p",1,MSG_NOSIGNAL,reinterpret_cast<sockaddr*>(&address),sizeof(address));int error=errno;
    close(fd);
    if(denied && sent==-1 && error==EACCES){puts("API32_NETD_PACKET_BLOCK stage=send");return 0;}
    return !denied && sent==1?0:67;
}
int policy(const sp<INetd>& netd,const char* executable) {
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(9);
    inet_pton(AF_INET,"192.0.2.1",&address.sin_addr);
    int route=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);socklen_t size=sizeof(address);
    if(route<0 || connect(route,reinterpret_cast<sockaddr*>(&address),sizeof(address)) ||
        getsockname(route,reinterpret_cast<sockaddr*>(&address),&size)) {perror("API32_NETD_ROUTE");return 10;}
    close(route);
    int receiver=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);address.sin_port=0;
    if(receiver<0 || bind(receiver,reinterpret_cast<sockaddr*>(&address),sizeof(address)) ||
        getsockname(receiver,reinterpret_cast<sockaddr*>(&address),&size))return 11;
    char ip[INET_ADDRSTRLEN],port[16];
    if(!inet_ntop(AF_INET,&address.sin_addr,ip,sizeof(ip)) || !strncmp(ip,"127.",4))return 12;
    snprintf(port,sizeof(port),"%u",ntohs(address.sin_port));
    unsigned allowed=0,denied=0;
    auto send=[&](bool allow,bool requireSocketDenial=false) {
        pid_t child=fork();if(child<0)return false;
        if(!child) {
            if(setgid(99998) || setuid(99998))_exit(64);
            execl(executable,executable,requireSocketDenial?"packet-socket-denied":allow?"packet-allowed":"packet-denied",ip,port,nullptr);_exit(65);
        }
        int status=0;
        while(waitpid(child,&status,0)<0)if(errno!=EINTR)return false;
        if(!WIFEXITED(status) || WEXITSTATUS(status))return false;
        pollfd fd{receiver,POLLIN,0};int ready;
        do {ready=poll(&fd,1,allow?1000:150);}while(ready<0 && errno==EINTR);
        if(!allow){if(ready!=0)return false;++denied;return true;}
        char byte=0;if(ready!=1 || recv(receiver,&byte,1,0)!=1 || byte!='p')return false;
        ++allowed;return true;
    };
    auto grant=[&](int permission){return ok(netd->trafficSetNetPermForUids(permission,{99998}),"trafficPermission");};
    bool replaced=false;
    if(!grant(INetd::PERMISSION_INTERNET) || !send(true))return 13;
    if(!ok(netd->firewallReplaceUidChain("fw_standby",false,{99998},&replaced),"standbyReplace") || !replaced ||
       !ok(netd->firewallEnableChildChain(INetd::FIREWALL_CHAIN_STANDBY,true),"standbyEnable") || !send(false))return 14;
    if(!ok(netd->firewallEnableChildChain(INetd::FIREWALL_CHAIN_STANDBY,false),"standbyDisable") || !send(true))return 15;
    if(!ok(netd->firewallReplaceUidChain("fw_restricted",true,{},&replaced),"restrictedReplace") || !replaced ||
       !ok(netd->firewallEnableChildChain(INetd::FIREWALL_CHAIN_RESTRICTED,true),"restrictedEnable") || !send(false))return 16;
    if(!ok(netd->firewallSetUidRule(INetd::FIREWALL_CHAIN_RESTRICTED,99998,INetd::FIREWALL_RULE_ALLOW),"restrictedAllow") || !send(true))return 17;
    if(!ok(netd->firewallSetUidRule(INetd::FIREWALL_CHAIN_RESTRICTED,99998,INetd::FIREWALL_RULE_DENY),"restrictedDeny") || !send(false))return 18;
    if(!ok(netd->firewallEnableChildChain(INetd::FIREWALL_CHAIN_RESTRICTED,false),"restrictedDisable") || !send(true))return 19;
    if(!grant(INetd::NO_PERMISSIONS) || !send(false,true) || !grant(INetd::PERMISSION_INTERNET) || !send(true))return 20;
    if(!grant(INetd::NO_PERMISSIONS))return 21;
    close(receiver);
    printf("API32_NETD_POLICY_PASS real_udp_delivered=%u real_udp_blocked=%u standby=1 restricted=1 internet_permission=1 host_firewall=unchanged\n",allowed,denied);
    return 0;
}
}
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);
    if(argc>=2 && !strncmp(argv[1],"packet-",7))return packet(argc,argv);
    if(argc!=2 || getuid()!=0)return 64;
    bool denied=!strcmp(argv[1],"denied"),stack=!strcmp(argv[1],"stack"),check=!strcmp(argv[1],"check"),packets=!strcmp(argv[1],"policy");
    if(!denied && !stack && !check && !packets)return 64;
    if(!packets) {int uid=denied?39999:stack?1073:1000;if(setgid(uid) || setuid(uid))return 64;}
    auto netd=service();if(!netd || !IInterface::asBinder(netd)->isBinderAlive())return 1;
    bool alive=false;auto status=netd->isAlive(&alive);
    if(denied) {
        if(status.exceptionCode()!=binder::Status::EX_SECURITY)return 2;
        MarkMaskParcel mark;
        if(netd->getFwmarkForNetwork(100,&mark).exceptionCode()!=binder::Status::EX_SECURITY)return 27;
        puts("API32_NETD_ROLE_DENIED_PASS uid=39999 operation=isAlive");return 0;
    }
    if(!ok(status,"isAlive") || !alive || netd->getInterfaceVersion()!=7 ||
        netd->getInterfaceHash()!="850353de5d19a0dd718f8fd20791f0532e6a34c7")return 3;
    if(stack) {
        if(!fwmarkErrors())return 28;
        MarkMaskParcel mark;
        if(!specific(netd->getFwmarkForNetwork(100,&mark),EOPNOTSUPP))return 27;
        puts("API32_NETD_STACK_FWMARK_UNSUPPORTED_PASS uid=1073 host_marks=unchanged");
        auto denied=netd->firewallEnableChildChain(INetd::FIREWALL_CHAIN_STANDBY,true);
        if(denied.exceptionCode()!=binder::Status::EX_SECURITY)return 4;
        puts("API32_NETD_STACK_ROLE_PASS uid=1073 alive=allowed mutation=denied");return 0;
    }
    if(packets)return policy(netd,argv[0]);
    std::vector<std::string> interfaces;
    if(!ok(netd->interfaceGetList(&interfaces),"interfaceGetList") || !interfaces.empty())return 5;
    NativeNetworkConfig config;config.netId=64512;config.permission=INetd::PERMISSION_NONE;
    if(!ok(netd->networkCreate(config),"networkCreate") || !specific(netd->networkCreate(config),EEXIST))return 6;
    int32_t current=-1;
    if(!ok(netd->networkSetDefault(config.netId),"setDefault") || !ok(netd->networkGetDefault(&current),"getDefault") || current!=config.netId)return 7;
    int socketFd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);uint32_t before=0,after=0;socklen_t markSize=sizeof(before);
    if(socketFd<0 || getsockopt(socketFd,SOL_SOCKET,SO_MARK,&before,&markSize) ||
        mark(socketFd,FwmarkCommand::SELECT_NETWORK,config.netId)!=0 ||
        mark(socketFd,FwmarkCommand::SELECT_NETWORK,64513)!=-ENONET ||
        mark(socketFd,FwmarkCommand::PROTECT_FROM_VPN,0)!=-EOPNOTSUPP ||
        mark(socketFd,FwmarkCommand::ON_CONNECT,0,false)!=-EINVAL ||
        getsockopt(socketFd,SOL_SOCKET,SO_MARK,&after,&markSize) || before!=after)return 26;
    close(socketFd);
    puts("API32_FWMARK_PASS private_socket=1 peer_identity=1 descriptor=validated host_mark=unchanged vpn_bypass=unsupported");
    if(!ok(netd->networkDestroy(config.netId),"destroy") || !ok(netd->networkGetDefault(&current),"getDefault") || current!=0 ||
        !specific(netd->networkSetDefault(config.netId),ENONET))return 8;
    config.netId=-1;
    if(!specific(netd->networkCreate(config),EINVAL) || !specific(netd->ipfwdEnableForwarding("probe"),EOPNOTSUPP))return 9;
    puts("API32_NETD_PASS typed_aidl=7 hash=verified network_lifecycle=1 invalid_rejected=1 host_privileged_operation=unsupported");return 0;
}
