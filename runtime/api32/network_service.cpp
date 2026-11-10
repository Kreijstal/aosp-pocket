#include "service_output.h"
// SPDX-License-Identifier: GPL-3.0-only
// Typed 12L guest networking. Host routes, firewall, VPN and interfaces remain
// owned by Android; guest UID traffic rules are enforced by Pocket's loader.
#include "NetdUnsupported32.h"
#include "binder_listeners.h"
#include "received_rights.h"
#include "../loader/network_policy.h"
#include "../api28/binder_identity.h"
#include "kernel_identity.h"
#include <FwmarkCommand.h>
#include <binder/IServiceManager.h>
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <string.h>

using namespace android;
using namespace android::net;
using binder::Status;
namespace {
Status error(int value,const char* message) {return Status::fromServiceSpecificError(value,message);}
bool validUid(int uid) {return uid>=0 && static_cast<unsigned>(uid)<POCKET_NP_UIDS;}
bool validUids(const std::vector<int32_t>& uids) {
    return uids.size()<=POCKET_NP_UIDS && std::all_of(uids.begin(),uids.end(),validUid);
}
int policyChain(int chain) {
    if(chain>=INetd::FIREWALL_CHAIN_DOZABLE && chain<=INetd::FIREWALL_CHAIN_POWERSAVE)return chain-1;
    if(chain==INetd::FIREWALL_CHAIN_RESTRICTED)return POCKET_NP_RESTRICTED;
    return -1;
}
class Netd32 final : public NetdUnsupported32 {
    std::mutex mutex;
    PocketNetworkPolicyV3 policy{};
    std::map<int,int> networks;
    int defaultNetwork=0;
    std::map<int,int> userPermissions;
    sp<BinderListeners32<INetdUnsolicitedEventListener>> listeners=new BinderListeners32<INetdUnsolicitedEventListener>;
    Status publish(const PocketNetworkPolicyV3& next) {
        std::string temporary=std::string(POCKET_NP_PATH)+".tmp."+std::to_string(getpid());
        int fd=open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd<0)return error(errno,"Cannot create guest network policy");
        const char* bytes=reinterpret_cast<const char*>(&next);size_t remaining=sizeof(next);int failed=0;
        while(remaining) {
            ssize_t count=write(fd,bytes,remaining);
            if(count<0 && errno==EINTR)continue;
            if(count<=0){failed=count<0?errno:EIO;break;}
            bytes+=count;remaining-=count;
        }
        if(!failed && fsync(fd))failed=errno;
        if(close(fd) && !failed)failed=errno;
        if(!failed && rename(temporary.c_str(),POCKET_NP_PATH))failed=errno;
        if(failed){unlink(temporary.c_str());return error(failed,"Cannot publish guest network policy");}
        policy=next;
        fprintf(stderr,"API32_NETD_POLICY_APPLIED version=3 enabled=%u host_firewall=unchanged\n",next.header.enabled);
        return Status::ok();
    }
    Status create(int netId,int permission) {
        if(netId<100 || netId>65535 || permission<INetd::PERMISSION_NONE || permission>INetd::PERMISSION_SYSTEM)
            return error(EINVAL,"Invalid guest network configuration");
        std::lock_guard<std::mutex> lock(mutex);
        if(networks.count(netId))return error(EEXIST,"Guest network already exists");
        networks.emplace(netId,permission);return Status::ok();
    }
public:
    int selectNetwork(unsigned netId,unsigned uid) {
        if(uid>=POCKET_NP_UIDS)return -EACCES;
        std::lock_guard<std::mutex> lock(mutex);
        if(policy.members[POCKET_NP_NO_INTERNET][uid/8]&(1U<<(uid%8)))return -EACCES;
        if(!netId)return 0; // Shared host-default path, without host SO_MARK.
        auto network=networks.find(netId);if(network==networks.end())return -ENONET;
        auto permission=userPermissions.find(uid);
        int granted=uid<10000?INetd::PERMISSION_SYSTEM:permission==userPermissions.end()?0:permission->second;
        return granted>=network->second?0:-EACCES;
    }
    Status initialize() {
        policy.header={POCKET_NP_MAGIC,3,1U<<POCKET_NP_NO_INTERNET,0};
        // Applications start denied until the framework supplies their INTERNET
        // grants. Logical system services retain their bootstrap connectivity.
        for(unsigned uid=10000;uid<POCKET_NP_UIDS;++uid)
            policy.members[POCKET_NP_NO_INTERNET][uid/8]|=1U<<(uid%8);
        return publish(policy);
    }
    status_t onTransact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags) override {
        if(code>=IBinder::FIRST_CALL_TRANSACTION && code<=IBinder::LAST_CALL_TRANSACTION) {
            uid_t uid=IPCThreadState::self()->getCallingUid();
            bool networkStack=uid==1073 && (code==TRANSACTION_isAlive ||
                code==TRANSACTION_registerUnsolicitedEventListener || code==TRANSACTION_getInterfaceVersion ||
                code==TRANSACTION_getInterfaceHash || code==TRANSACTION_getFwmarkForNetwork);
            if(uid!=0 && uid!=1000 && !networkStack) {
                if(!reply)return PERMISSION_DENIED;
                return Status::fromExceptionCode(Status::EX_SECURITY,"Guest netd requires a system role").writeToParcel(reply);
            }
        }
        return BnNetd::onTransact(code,data,reply,flags);
    }
    Status isAlive(bool* value) override {*value=true;return Status::ok();}
    Status registerUnsolicitedEventListener(const sp<INetdUnsolicitedEventListener>& listener) override {
        auto result=listeners->add(listener);
        if(result.isOk())puts("API32_NETD_LISTENER_REGISTERED typed=1 live=1");
        return result;
    }
    Status interfaceGetList(std::vector<std::string>* value) override {
        // The host uplink agent has not published any guest interfaces yet.
        value->clear();return Status::ok();
    }
    Status networkCreatePhysical(int32_t netId,int32_t permission) override {return create(netId,permission);}
    Status networkCreate(const NativeNetworkConfig& config) override {
        if(config.networkType!=NativeNetworkType::PHYSICAL || config.secure)
            return unsupportedNetd32("networkCreateVpn");
        return create(config.netId,config.permission);
    }
    Status networkDestroy(int32_t netId) override {
        std::lock_guard<std::mutex> lock(mutex);
        if(!networks.erase(netId))return error(ENONET,"Unknown guest network");
        if(defaultNetwork==netId)defaultNetwork=0;
        return Status::ok();
    }
    Status networkSetDefault(int32_t netId) override {
        std::lock_guard<std::mutex> lock(mutex);
        if(!networks.count(netId))return error(ENONET,"Unknown guest network");
        defaultNetwork=netId;return Status::ok();
    }
    Status networkGetDefault(int32_t* value) override {
        std::lock_guard<std::mutex> lock(mutex);*value=defaultNetwork;return Status::ok();
    }
    Status networkClearDefault() override {std::lock_guard<std::mutex> lock(mutex);defaultNetwork=0;return Status::ok();}
    Status networkSetPermissionForNetwork(int32_t netId,int32_t permission) override {
        if(permission<INetd::PERMISSION_NONE || permission>INetd::PERMISSION_SYSTEM)return error(EINVAL,"Invalid permission");
        std::lock_guard<std::mutex> lock(mutex);auto found=networks.find(netId);
        if(found==networks.end())return error(ENONET,"Unknown guest network");
        found->second=permission;return Status::ok();
    }
    Status networkSetPermissionForUser(int32_t permission,const std::vector<int32_t>& uids) override {
        if(!validUids(uids) || (permission!=INetd::PERMISSION_NETWORK && permission!=INetd::PERMISSION_SYSTEM))
            return error(EINVAL,"Invalid UID permission request");
        std::lock_guard<std::mutex> lock(mutex);for(int uid:uids)userPermissions[uid]=permission;
        return Status::ok();
    }
    Status networkClearPermissionForUser(const std::vector<int32_t>& uids) override {
        if(!validUids(uids))return error(EINVAL,"Invalid UID list");
        std::lock_guard<std::mutex> lock(mutex);for(int uid:uids)userPermissions.erase(uid);
        return Status::ok();
    }
    Status trafficSetNetPermForUids(int32_t permission,const std::vector<int32_t>& uids) override {
        if(!validUids(uids) || (permission!=INetd::PERMISSION_UNINSTALLED && (permission<0 || (permission&~12))))
            return error(EINVAL,"Invalid traffic permission request");
        std::lock_guard<std::mutex> lock(mutex);auto next=policy;
        for(int uid:uids) {
            auto& byte=next.members[POCKET_NP_NO_INTERNET][uid/8];
            if(permission!=INetd::PERMISSION_UNINSTALLED && !(permission&INetd::PERMISSION_INTERNET))byte|=1U<<(uid%8);
            else byte&=~(1U<<(uid%8));
        }
        return publish(next);
    }
    Status firewallReplaceUidChain(const std::string& name,bool allowlist,const std::vector<int32_t>& uids,bool* value) override {
        int chain=-1;
        if(name=="fw_dozable")chain=POCKET_NP_DOZE;
        else if(name=="fw_standby")chain=POCKET_NP_STANDBY;
        else if(name=="fw_powersave")chain=POCKET_NP_POWERSAVE;
        else if(name=="fw_restricted")chain=POCKET_NP_RESTRICTED;
        if(chain<0 || allowlist!=(chain!=POCKET_NP_STANDBY) || !validUids(uids))return error(EINVAL,"Invalid guest firewall chain");
        std::lock_guard<std::mutex> lock(mutex);auto next=policy;
        memset(next.members[chain],0,POCKET_NP_BYTES);
        for(int uid:uids)next.members[chain][uid/8]|=1U<<(uid%8);
        auto status=publish(next);if(status.isOk())*value=true;return status;
    }
    Status firewallEnableChildChain(int32_t childChain,bool enabled) override {
        int chain=policyChain(childChain);if(chain<0)return error(EINVAL,"Unknown guest firewall chain");
        std::lock_guard<std::mutex> lock(mutex);auto next=policy;
        if(enabled)next.header.enabled|=1U<<chain;else next.header.enabled&=~(1U<<chain);
        return publish(next);
    }
    Status firewallSetUidRule(int32_t childChain,int32_t uid,int32_t rule) override {
        int chain=policyChain(childChain);
        if(chain<0 || !validUid(uid) || (rule!=INetd::FIREWALL_RULE_ALLOW && rule!=INetd::FIREWALL_RULE_DENY))
            return error(EINVAL,"Invalid guest firewall rule");
        std::lock_guard<std::mutex> lock(mutex);auto next=policy;
        bool member=chain==POCKET_NP_STANDBY?rule==INetd::FIREWALL_RULE_DENY:rule==INetd::FIREWALL_RULE_ALLOW;
        auto& byte=next.members[chain][uid/8];
        if(member)byte|=1U<<(uid%8);else byte&=~(1U<<(uid%8));
        return publish(next);
    }
    Status firewallSetFirewallType(int32_t type) override {
        return type==INetd::FIREWALL_DENYLIST?Status::ok():unsupportedNetd32("firewallSetFirewallTypeAllowlist");
    }
    Status bandwidthEnableDataSaver(bool enable,bool* value) override {
        if(enable)return unsupportedNetd32("bandwidthEnableDataSaver");
        *value=true;return Status::ok();
    }
    Status ipfwdEnabled(bool* value) override {*value=false;return Status::ok();}
    Status ipfwdGetRequesterList(std::vector<std::string>* value) override {value->clear();return Status::ok();}
    Status tetherIsEnabled(bool* value) override {*value=false;return Status::ok();}
    Status tetherInterfaceList(std::vector<std::string>* value) override {value->clear();return Status::ok();}
    Status networkCanProtect(int32_t uid,bool* value) override {
        if(!validUid(uid))return error(EINVAL,"Invalid UID");
        *value=false;return Status::ok();
    }
};
int fwmarkSocket() {
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -1;
    sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/fwmarkd");
    unlink(address.sun_path);
    if(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || chmod(address.sun_path,0666) || listen(fd,16)) {
        close(fd);return -1;
    }
    return fd;
}
void serveFwmark(int server,const sp<Netd32>& service) {
    const uid_t kernelUid=pocket_kernel_uid_from_proc();
    for(;;) {
        int fd=accept4(server,nullptr,nullptr,SOCK_CLOEXEC);
        if(fd<0){if(errno==EINTR)continue;return;}
        timeval timeout{2,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        ucred peer{};socklen_t peerSize=sizeof(peer);uint64_t identity=0;
        bool trusted=kernelUid!=static_cast<uid_t>(-1) &&
            !getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&peerSize) && peer.uid==kernelUid &&
            pocket_binder_identity(peer.pid,&identity);
        char bytes[sizeof(FwmarkCommand)+sizeof(FwmarkConnectInfo)]{};
        alignas(cmsghdr) char control[CMSG_SPACE(8*sizeof(int))]{};
        iovec buffer{bytes,sizeof(bytes)};msghdr message{};
        message.msg_iov=&buffer;message.msg_iovlen=1;message.msg_control=control;message.msg_controllen=sizeof(control);
        ssize_t count;do{count=recvmsg(fd,&message,MSG_CMSG_CLOEXEC);}while(count<0 && errno==EINTR);
        const int receiveError=count<0?errno:0;std::vector<int> descriptors;
        bool internetSocket=pocket::decodeRights32(message,count,receiveError,sizeof(control),&descriptors)==0;
        for(int received:descriptors) {
            int domain=0;socklen_t domainSize=sizeof(domain);
            if(getsockopt(received,SOL_SOCKET,SO_DOMAIN,&domain,&domainSize) || (domain!=AF_INET && domain!=AF_INET6))internetSocket=false;
            close(received);
        }
        int result=trusted?-EINVAL:-EACCES;
        if(trusted && internetSocket && descriptors.size()==1 && count>=static_cast<ssize_t>(sizeof(FwmarkCommand)) && !(message.msg_flags&(MSG_TRUNC|MSG_CTRUNC))) {
            FwmarkCommand command;memcpy(&command,bytes,sizeof(command));
            result=-EOPNOTSUPP;
            switch(command.cmdId) {
                case FwmarkCommand::SELECT_NETWORK:result=service->selectNetwork(command.netId,identity>>32);break;
                case FwmarkCommand::ON_ACCEPT:case FwmarkCommand::ON_CONNECT:case FwmarkCommand::ON_CONNECT_COMPLETE:
                case FwmarkCommand::ON_SENDMMSG:case FwmarkCommand::ON_SENDMSG:case FwmarkCommand::ON_SENDTO:
                    result=service->selectNetwork(0,identity>>32);break;
                default:break;
            }
        }
        send(fd,&result,sizeof(result),MSG_NOSIGNAL);close(fd);
    }
}
}
int main() {
    pocketServiceOutput32();char sdk[PROP_VALUE_MAX];
    if(getuid()!=0 || syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL ||
        __system_property_get("ro.build.version.sdk",sdk)<=0 || strcmp(sdk,"32"))return 64;
    sp<Netd32> service=new Netd32;
    auto initialized=service->initialize();
    if(!initialized.isOk()){fprintf(stderr,"API32_NETD_POLICY_ERROR %s\n",initialized.toString8().c_str());return 1;}
    int fwmark=fwmarkSocket();if(fwmark<0){perror("API32_FWMARK_SOCKET");return 3;}
    std::thread(serveFwmark,fwmark,service).detach();
    ProcessState::self()->startThreadPool();auto manager=defaultServiceManager();
    if(!manager || manager->addService(String16("netd"),service)!=NO_ERROR)return 2;
    puts("API32_NETD_READY typed_aidl=7 policy_version=3 host_kernel=unchanged");
    IPCThreadState::self()->joinThreadPool();return 0;
}
