// SPDX-License-Identifier: GPL-3.0-only
#include "power.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <atomic>
#include <stdio.h>
#include <string.h>
using namespace android;
using namespace android::hardware;
using namespace android::system::suspend::V1_0;
static bool transfer(int fd,void* data,size_t size,bool writing) {
    char* next=static_cast<char*>(data);
    while(size) {
        ssize_t count=writing?send(fd,next,size,MSG_NOSIGNAL):recv(fd,next,size,0);
        if(count<0 && errno==EINTR)continue;
        if(count<=0)return false;
        next+=count;size-=count;
    }
    return true;
}
int pocketPowerRequest(unsigned operation,uint64_t token,const char* name,uint64_t* answer) {
    size_t length=name?strlen(name):0;if(length>256)return -EINVAL;
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -errno;
    sockaddr_un address={};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/pocket_power");
    timeval timeout={2,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    uint32_t header[]={htonl(0x504b5031),htonl(operation),htonl(length),htonl(token>>32),htonl(token)};
    uint32_t reply[3]={};int result=-EIO;
    if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)))result=-errno;
    else if(transfer(fd,header,sizeof(header),true) && (!length || transfer(fd,const_cast<char*>(name),length,true)) &&
            transfer(fd,reply,sizeof(reply),false)) {
        result=-static_cast<int>(ntohl(reply[0]));
        if(answer)*answer=(static_cast<uint64_t>(ntohl(reply[1]))<<32)|ntohl(reply[2]);
    }
    close(fd);return result;
}
namespace {
class RelayWakeLock final : public IWakeLock {
    uint64_t token;
    std::atomic<bool> released{false};
public:
    explicit RelayWakeLock(uint64_t token):token(token){}
    ~RelayWakeLock() override {release();}
    Return<void> release() override {
        if(!released.exchange(true)) {
            int result=pocketPowerRequest(2,token,nullptr,nullptr);
            if(result)fprintf(stderr,"API29_WAKE_LOCK_RELEASE_FAILED status=%d\n",result);
        }
        return Void();
    }
};
class SystemSuspend final : public ISystemSuspend {
    Return<sp<IWakeLock>> acquireWakeLock(WakeLockType type,const hidl_string& name) override {
        if(type!=WakeLockType::PARTIAL && type!=WakeLockType::FULL)return sp<IWakeLock>();
        // The HIDL contract permits CPU-only behavior for FULL. Both types
        // hold a real host PARTIAL_WAKE_LOCK; host screen policy is untouched.
        uint64_t token=0;int result=pocketPowerRequest(1,0,name.c_str(),&token);
        if(result || !token) {
            fprintf(stderr,"API29_WAKE_LOCK_ACQUIRE_FAILED status=%d\n",result);
            return sp<IWakeLock>();
        }
        return sp<IWakeLock>(new RelayWakeLock(token));
    }
};
}
sp<ISystemSuspend> pocketSystemSuspend() {static sp<ISystemSuspend> service=new SystemSuspend;return service;}
