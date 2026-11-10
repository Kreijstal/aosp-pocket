// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "external_open_rpc.h"
#include <functional>
#include <stdio.h>
#include <sys/random.h>

namespace pocket::fuse32 {
constexpr uint32_t VolumeMagic=0x32565850;
struct VolumeRequest {uint32_t magic,version,operation,reserved;uint64_t epoch;};
struct VolumeReply {uint32_t magic,version;int32_t error;uint32_t reserved;uint64_t epoch;};
static_assert(sizeof(VolumeRequest)==24 && sizeof(VolumeReply)==24);

// One userspace emulated-volume session, embedded in the UID0 storage core.
// The framework callback starts the real ExternalStorageService while INIT is
// in flight. Host commands attach/revoke a real AppFuse adapter; failure at
// either boundary cannot leave an advertised mounted session behind.
class ExternalVolume final {
public:
    struct Options {
        pid_t hostPid=0;uid_t hostUid=0;uint64_t hostStart=0;
        std::string directory="/dev/socket",identity="/.pocket-identity",locator="/.pocket-external-ready",device="/.pocket-external-device";
        std::string lower="/data/media";
    };
    using Checking=std::function<int(int)>;
    using Adapter=std::function<int(unsigned,uint64_t)>;
private:
    const Options options_;
    const Adapter adapter_;
    std::mutex operations_;
    std::unique_ptr<Channel> channel_;
    std::unique_ptr<Files> files_;
    std::unique_ptr<HostFiles> host_;
    std::unique_ptr<AppFiles> apps_;
    uint64_t epoch_=0;
    bool adapterRequested_=false,mounted_=false;
    int stopLocked() {
        mounted_=false;int result=0;
        const bool active=channel_ || adapterRequested_;
        auto trace=[&](const char* stage){if(active)fprintf(stderr,"API32_EXTERNAL_VOLUME_STOP stage=%s result=%d\n",stage,result);};
        trace("begin");
        // Stop new path requests, then revoke the kernel proxy mount while
        // its native callback broker is still alive for final cap releases.
        if(apps_)apps_->stop();
        trace("app_broker_stopped");
        if(adapterRequested_){result=adapter_(2,epoch_);if(!result)adapterRequested_=false;}
        trace("host_adapter_revoked");
        // A timed-out reply does not mean the host stopped using its file
        // capabilities. Keep the callback broker and provider channel alive
        // for that in-flight cleanup and an idempotent detach confirmation.
        if(result<0){trace("detach_pending");return result;}
        if(host_)host_->stop();
        trace("host_broker_stopped");
        if(files_)files_->closeAll();
        trace("file_handles_closed");
        apps_.reset();host_.reset();files_.reset();
        trace("brokers_released");
        if(channel_)channel_->cancel();
        trace("channel_cancelled");
        channel_.reset();if(!adapterRequested_)epoch_=0;
        trace("complete");
        return result;
    }
public:
    ExternalVolume(Options options,Adapter adapter):options_(std::move(options)),adapter_(std::move(adapter)){}
    ExternalVolume(const ExternalVolume&)=delete;
    ~ExternalVolume(){stop();}
    int mount(const Checking& checking,Identity initializer) {
        std::lock_guard<std::mutex> lock(operations_);
        if(channel_ || adapterRequested_)return -EBUSY;
        if(!checking || !adapter_ || options_.hostPid<=0 || !options_.hostStart)return -EINVAL;
        int pair[2];if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair))return -errno;
        channel_=std::make_unique<Channel>(pair[0],15000);files_=std::make_unique<Files>(*channel_,options_.lower);
        int initialized=-EIO;std::thread initialization([&]{initialized=files_->initialize(initializer);});
        int accepted=checking(pair[1]);close(pair[1]);
        if(accepted<0)channel_->cancel();
        initialization.join();
        auto fail=[&](int error){stopLocked();return error;};
        if(accepted<0)return fail(accepted);
        if(initialized<0)return fail(initialized);
        ssize_t n;do{n=getrandom(&epoch_,sizeof(epoch_),0);}while(n<0 && errno==EINTR);
        if(n!=sizeof(epoch_) || !epoch_)return fail(n<0?-errno:-EIO);
        host_=std::make_unique<HostFiles>(*files_,options_.hostPid,options_.hostUid,options_.hostStart);
        int result=host_->start(options_.directory+"/pocket_ext");if(result<0)return fail(result);
        apps_=std::make_unique<AppFiles>(*files_,options_.hostPid,options_.hostUid,options_.hostStart,options_.identity);
        result=apps_->start(options_.directory+"/pocket_efs",options_.directory+"/pocket_xfd",options_.locator,options_.device);if(result<0)return fail(result);
        // Even a lost success reply requires an idempotent detach: the host
        // may already own a live adapter by the time the request disconnects.
        adapterRequested_=true;result=adapter_(1,epoch_);if(result<0)return fail(result);
        uint64_t device=0;n=readIdentityFile(options_.device.c_str(),&device,sizeof(device));
        if(n!=sizeof(device) || !device)return fail(n<0?static_cast<int>(n):-ENODEV);
        mounted_=true;return 0;
    }
    int stop(){std::lock_guard<std::mutex> lock(operations_);return stopLocked();}
    bool mounted(){std::lock_guard<std::mutex> lock(operations_);return mounted_;}
    bool clean(){std::lock_guard<std::mutex> lock(operations_);return !channel_ && !adapterRequested_;}
};

inline int volumeAdapter(const ExternalVolume::Options& options,unsigned operation,uint64_t epoch,int timeoutMs=20000) {
    if(timeoutMs<=0 || timeoutMs>20000)return -EINVAL;
    auto trace=[&](const char* stage,int result=0){fprintf(stderr,"API32_EXTERNAL_VOLUME_ADAPTER operation=%u stage=%s result=%d\n",operation,stage,result);};
    trace("begin");
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -errno;
    auto finish=[&](int rc){trace("closing",rc);close(fd);trace("complete",rc);return rc;};
    timeval deadline{timeoutMs/1000,(timeoutMs%1000)*1000};
    if(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&deadline,sizeof(deadline)) || setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&deadline,sizeof(deadline)))return finish(-errno);
    sockaddr_un address{};address.sun_family=AF_UNIX;auto path=options.directory+"/pocket_volume32";
    if(path.size()>=sizeof(address.sun_path))return finish(-ENAMETOOLONG);
    memcpy(address.sun_path,path.c_str(),path.size()+1);
    if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)))return finish(-errno);
    if(hostPeer(fd,options.hostPid,options.hostUid)<0)return finish(-EACCES);
    trace("connected");
    VolumeRequest request{VolumeMagic,1,operation,0,epoch};VolumeReply reply{};
    auto exact=[&](void* bytes,size_t size,bool writing){size_t at=0;while(at<size){ssize_t n=writing?send(fd,static_cast<char*>(bytes)+at,size-at,MSG_NOSIGNAL):recv(fd,static_cast<char*>(bytes)+at,size-at,0);if(n<0 && errno==EINTR)continue;if(n<=0)return n<0?-errno:-ECONNRESET;at+=n;}return 0;};
    int result=exact(&request,sizeof(request),true);std::vector<int> rights;
    trace("request_sent",result);
    if(!result) {
        char control[CMSG_SPACE(8*sizeof(int))]{};iovec part{&reply,sizeof(reply)};msghdr message{};message.msg_iov=&part;message.msg_iovlen=1;message.msg_control=control;message.msg_controllen=sizeof(control);
        ssize_t count;do{count=recvmsg(fd,&message,MSG_CMSG_CLOEXEC);}while(count<0 && errno==EINTR);
        const int receivedError=count<0?errno:0;trace("reply_received",-receivedError);
        result=pocket::decodeRights32(message,count,receivedError,sizeof(control),&rights);
        if(!result && count<static_cast<ssize_t>(sizeof(reply)))result=exact(reinterpret_cast<char*>(&reply)+count,sizeof(reply)-count,false);
    }
    if(!result)result=reply.magic!=VolumeMagic || reply.version!=1 || reply.reserved || reply.epoch!=epoch || reply.error>0 || reply.error<-4095?-EPROTO:reply.error;
    if(!result && (rights.size()!=1 || !lifetimeDescriptor(rights[0],options.hostPid,options.hostStart)))result=-EACCES;
    else if(result<0 && !rights.empty() && reply.error)result=-EPROTO;
    trace("reply_validated",result);
    for(int value:rights)close(value);
    return finish(result);
}
}
