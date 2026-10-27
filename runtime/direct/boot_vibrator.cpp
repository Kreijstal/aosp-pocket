// SPDX-License-Identifier: GPL-3.0-only
// In-process Vibrator 1.2 HAL that forwards to the host broker's VIBRATE operation:
// on()/off(), amplitude when the host vibrator has amplitude control, and predefined
// effects with their strength. The host decides availability; an absent host
// vibrator or effect is reported, not faked, so VibratorService uses its fallback.
#include "boot_vibrator.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <android/hardware/vibrator/1.2/IVibrator.h>
#include <mutex>
#include <time.h>
using namespace android;
using namespace android::hardware;
using android::hardware::vibrator::V1_0::Effect;
using android::hardware::vibrator::V1_0::EffectStrength;
using android::hardware::vibrator::V1_0::IVibrator;
using android::hardware::vibrator::V1_1::Effect_1_1;
using IVibrator_1_2=android::hardware::vibrator::V1_2::IVibrator;
using Effect_1_2=android::hardware::vibrator::V1_2::Effect;
using VibratorStatus=android::hardware::vibrator::V1_0::Status;
namespace {
// Must match dev.kreijstal.aosppocket.bridge.HostWire.
constexpr uint32_t kMagic=0x504b4831,kVibrate=11,kMaxMilliseconds=30000;
bool sendAll(int fd,const void* data,size_t size) {
    auto* bytes=static_cast<const uint8_t*>(data);
    while(size) {ssize_t n=send(fd,bytes,size,MSG_NOSIGNAL);if(n<=0)return false;bytes+=n;size-=n;}
    return true;
}
bool readAll(int fd,void* data,size_t size) {
    auto* bytes=static_cast<uint8_t*>(data);
    while(size) {ssize_t n=recv(fd,bytes,size,0);if(n<=0)return false;bytes+=n;size-=n;}
    return true;
}
// Sends one request; returns the host status (0 ok, 404 unsupported, ...) or -errno on transport failure.
// The reply body, if any, is copied NUL-terminated into reply.
int hostRequest(const char* payload,char* reply,size_t size) {
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -errno;
    sockaddr_un address{};address.sun_family=AF_UNIX;
    strncpy(address.sun_path,"/dev/socket/pocket_host",sizeof(address.sun_path)-1);
    // Vibrator calls run under VibratorService's lock; never wait long on the host.
    timeval timeout{2,0};
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    uint32_t length=strlen(payload),header[3]={htonl(kMagic),htonl(kVibrate),htonl(length)};
    int status=-EIO;uint32_t head[2];reply[0]=0;
    if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0) status=-errno;
    else if(sendAll(fd,header,sizeof(header)) && sendAll(fd,payload,length) && readAll(fd,head,sizeof(head))) {
        uint32_t body=ntohl(head[1]);
        if(body<size && readAll(fd,reply,body)) {reply[body]=0;status=ntohl(head[0]);}
    }
    close(fd);
    return status;
}
int hostVibrate(uint32_t milliseconds,uint8_t amplitude,char* reply,size_t size) {
    char payload[64];
    // Amplitude 0 asks for the host default.
    if(amplitude) snprintf(payload,sizeof(payload),"{\"milliseconds\":%u,\"amplitude\":%u}",milliseconds,amplitude);
    else snprintf(payload,sizeof(payload),"{\"milliseconds\":%u}",milliseconds);
    return hostRequest(payload,reply,size);
}
VibratorStatus report(const char* call,int status) {
    if(status==0)return VibratorStatus::OK;
    fprintf(stderr,"POCKET_VIBRATOR_HOST_UNAVAILABLE call=%s status=%d\n",call,status);
    return status==404?VibratorStatus::UNSUPPORTED_OPERATION:VibratorStatus::UNKNOWN_ERROR;
}
int64_t nowMs() {timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000LL+t.tv_nsec/1000000;}
class HostVibrator final : public IVibrator_1_2 {
public:
    explicit HostVibrator(bool amplitudeControl):amplitudeControl(amplitudeControl) {}
    Return<VibratorStatus> on(uint32_t timeoutMs) override {
        if(!timeoutMs)return VibratorStatus::BAD_VALUE;
        std::lock_guard<std::mutex> hold(lock);char reply[256];
        uint32_t duration=timeoutMs<kMaxMilliseconds?timeoutMs:kMaxMilliseconds;
        int status=hostVibrate(duration,amplitude,reply,sizeof(reply));
        until=status==0?nowMs()+duration:0;
        return report("on",status);
    }
    Return<VibratorStatus> off() override {
        std::lock_guard<std::mutex> hold(lock);char reply[256];until=0;
        return report("off",hostVibrate(0,0,reply,sizeof(reply)));
    }
    Return<bool> supportsAmplitudeControl() override {return amplitudeControl;}
    Return<VibratorStatus> setAmplitude(uint8_t value) override {
        if(!amplitudeControl)return VibratorStatus::UNSUPPORTED_OPERATION;
        if(!value)return VibratorStatus::BAD_VALUE;
        std::lock_guard<std::mutex> hold(lock);amplitude=value;
        // Waveforms change amplitude while on(); restart the host one-shot for the remaining time.
        int64_t remaining=until-nowMs();if(remaining<=0)return VibratorStatus::OK;
        char reply[256];
        return report("setAmplitude",hostVibrate(static_cast<uint32_t>(remaining),value,reply,sizeof(reply)));
    }
    Return<void> perform(Effect effect,EffectStrength strength,perform_cb cb) override {
        return performEffect(static_cast<uint32_t>(effect),strength,cb);
    }
    Return<void> perform_1_1(Effect_1_1 effect,EffectStrength strength,perform_1_1_cb cb) override {
        return performEffect(static_cast<uint32_t>(effect),strength,cb);
    }
    Return<void> perform_1_2(Effect_1_2 effect,EffectStrength strength,perform_1_2_cb cb) override {
        return performEffect(static_cast<uint32_t>(effect),strength,cb);
    }
private:
    template<class Callback>
    Return<void> performEffect(uint32_t effect,EffectStrength strength,Callback& cb) {
        std::lock_guard<std::mutex> hold(lock);
        char payload[64],reply[256];
        snprintf(payload,sizeof(payload),"{\"effect\":%u,\"strength\":%u}",effect,static_cast<uint32_t>(strength));
        int status=hostRequest(payload,reply,sizeof(reply));
        const char* field=strstr(reply,"\"length_ms\":");
        uint32_t length=field?static_cast<uint32_t>(strtoul(field+12,nullptr,10)):0;
        if(status==0 && length) {
            until=nowMs()+length;
            fprintf(stderr,"POCKET_VIBRATOR_EFFECT effect=%u strength=%u length_ms=%u\n",effect,static_cast<uint32_t>(strength),length);
            cb(VibratorStatus::OK,length);
        } else {
            // 404: the host has no matching effect; VibratorService then plays its fallback waveform.
            fprintf(stderr,"POCKET_VIBRATOR_EFFECT effect=%u strength=%u status=%d\n",effect,static_cast<uint32_t>(strength),status);
            cb(status==404?VibratorStatus::UNSUPPORTED_OPERATION:VibratorStatus::UNKNOWN_ERROR,0);
        }
        return Void();
    }
    const bool amplitudeControl;
    std::mutex lock;
    uint8_t amplitude=0;   // 0 until VibratorService sets one: host default
    int64_t until=0;       // monotonic end of the current one-shot
};
}
sp<IVibrator> pocketHostVibrator() {
    // A cancel is harmless and tells whether the host has a usable vibrator,
    // so Vibrator.hasVibrator() in the guest matches the host.
    // Probe once per process: each interface version is fetched separately, and a later
    // cancel would cut off a vibration that is already playing.
    static sp<IVibrator> vibrator=[]() -> sp<IVibrator> {
        char reply[256];int status=hostVibrate(0,0,reply,sizeof(reply));
        bool amplitude=strstr(reply,"\"amplitude_control\":true")!=nullptr;
        fprintf(stderr,"POCKET_VIBRATOR_HAL host_status=%d available=%d amplitude_control=%d version=1.2\n",status,status==0,amplitude);
        return status==0?new HostVibrator(amplitude):nullptr;
    }();
    return vibrator;
}
