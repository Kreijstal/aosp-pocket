// SPDX-License-Identifier: GPL-3.0-only
// Typed native Binder callbacks/stats must follow real host partial wake locks.
#include <android/system/suspend/ISuspendControlService.h>
#include <android/system/suspend/BnSuspendCallback.h>
#include <android/system/suspend/BnWakelockCallback.h>
#include <android/system/suspend/internal/ISuspendControlServiceInternal.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include "../api29/power.h"
#include <atomic>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

using namespace android;
using namespace android::system::suspend;
using namespace android::system::suspend::internal;
namespace {
class WakelockCallback final : public BnWakelockCallback {
public:
    std::atomic<unsigned> acquired{0},released{0};
    binder::Status notifyAcquired() override {++acquired;return binder::Status::ok();}
    binder::Status notifyReleased() override {++released;return binder::Status::ok();}
};
class SuspendCallback final : public BnSuspendCallback {
public:
    std::atomic<unsigned> wakeups{0};
    binder::Status notifyWakeup(bool,const std::vector<std::string>&) override {
        ++wakeups;return binder::Status::ok();
    }
};
bool waitFor(const std::atomic<unsigned>& actual,unsigned expected) {
    for(unsigned i=0;i<250;++i) {
        if(actual.load()==expected)return true;
        usleep(20000);
    }
    return false;
}
bool held(uint64_t expected) {
    uint64_t count=0;
    return !pocketPowerRequest(3,0,nullptr,&count) && count==expected;
}
bool nativeStat(const sp<ISuspendControlServiceInternal>& service,const std::string& name,
        bool active,int64_t acquisitions) {
    std::vector<WakeLockInfo> entries;
    if(!service->getWakeLockStats(&entries).isOk())return false;
    for(const auto& entry:entries) {
        if(entry.name!=name || entry.pid!=getpid())continue;
        bool ok=!entry.isKernelWakelock && entry.isActive==active &&
            entry.activeCount==acquisitions && entry.totalTime>0 && entry.maxTime>0 &&
            (active?entry.activeTime>0:entry.activeTime==0) && entry.eventCount==0 &&
            entry.expireCount==0 && entry.preventSuspendTime==0 && entry.wakeupCount==0;
        printf("API32_SUSPEND_NATIVE_STAT name=%s pid=%d active=%d count=%lld total_ms=%lld max_ms=%lld valid=%d\n",
            name.c_str(),entry.pid,entry.isActive,(long long)entry.activeCount,
            (long long)entry.totalTime,(long long)entry.maxTime,ok);
        return ok;
    }
    return false;
}
}
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);
    bool denied=argc==2 && !strcmp(argv[1],"denied");
    if(argc!=2 || (!denied && strcmp(argv[1],"check")) || getuid()!=0)return 64;
    if(setgid(denied?39999:1000) || setuid(denied?39999:1000))return 64;
    android::ProcessState::self()->startThreadPool();
    auto manager=android::defaultServiceManager();if(!manager)return 1;
    auto control=interface_cast<ISuspendControlService>(manager->checkService(String16("suspend_control")));
    auto internal=interface_cast<ISuspendControlServiceInternal>(manager->checkService(String16("suspend_control_internal")));
    if(!control || !internal || !IInterface::asBinder(control)->remoteBinder() ||
            !IInterface::asBinder(internal)->remoteBinder())return 2;
    if(control->getInterfaceVersion()!=1 ||
            control->getInterfaceHash()!="34506b107801d68c881c2c7368ad4c676aed3e9b")return 3;
    sp<WakelockCallback> lockCallback=new WakelockCallback;
    sp<SuspendCallback> wakeCallback=new SuspendCallback;
    std::string name="Api32SuspendProbe:"+std::to_string(getpid());
    bool accepted=true;
    std::vector<WakeLockInfo> locks;
    std::vector<WakeupInfo> wakeups;
    SuspendInfo suspendInfo;
    if(denied) {
        for(const auto& status:{control->registerCallback(wakeCallback,&accepted),
                control->registerWakelockCallback(lockCallback,name,&accepted),
                internal->enableAutosuspend(&accepted),internal->forceSuspend(&accepted),
                internal->getWakeLockStats(&locks),internal->getWakeupStats(&wakeups),
                internal->getSuspendStats(&suspendInfo)}) {
            if(status.exceptionCode()!=binder::Status::EX_SECURITY)return 4;
        }
        puts("API32_SUSPEND_ROLE_DENIED_PASS uid=39999 methods=7 metadata_version=1");return 0;
    }
    // The frozen non-null AIDL rejects null during unmarshalling, before the
    // stock handler's defensive null check. Require that exact wire result.
    auto nullWake=control->registerCallback(nullptr,&accepted);
    auto nullLock=control->registerWakelockCallback(nullptr,name,&accepted);
    printf("API32_SUSPEND_NULL_CALLBACKS wake=%s lock=%s\n",
        nullWake.toString8().c_str(),nullLock.toString8().c_str());
    if(nullWake.exceptionCode()!=binder::Status::EX_NULL_POINTER ||
            nullLock.exceptionCode()!=binder::Status::EX_NULL_POINTER ||
            !control->registerWakelockCallback(lockCallback,"",&accepted).isOk() || accepted)return 5;
    if(!control->registerCallback(wakeCallback,&accepted).isOk() || !accepted ||
            !control->registerWakelockCallback(lockCallback,name,&accepted).isOk() || !accepted ||
            !control->registerWakelockCallback(lockCallback,name,&accepted).isOk() || accepted)return 6;
    if(!internal->enableAutosuspend(&accepted).isOk() || accepted ||
            !internal->forceSuspend(&accepted).isOk() || accepted)return 7;
    if(internal->getWakeupStats(&wakeups).exceptionCode()!=binder::Status::EX_UNSUPPORTED_OPERATION ||
            internal->getSuspendStats(&suspendInfo).exceptionCode()!=binder::Status::EX_UNSUPPORTED_OPERATION)return 8;
    auto suspend=V1_0::ISystemSuspend::getService("default");
    if(!suspend || !suspend->isRemote() || !held(0))return 9;
    for(unsigned round=1;round<=2;++round) {
        sp<V1_0::IWakeLock> lock=suspend->acquireWakeLock(V1_0::WakeLockType::PARTIAL,name);
        if(!lock || !held(1) || !waitFor(lockCallback->acquired,round))return 10;
        usleep(50000);
        if(!nativeStat(internal,name,true,round))return 11;
        if(!lock->release().isOk() || !waitFor(lockCallback->released,round) || !held(0) ||
                !nativeStat(internal,name,false,round))return 12;
        if(!lock->release().isOk())return 13;
        lock.clear();usleep(50000);
        if(lockCallback->acquired.load()!=round || lockCallback->released.load()!=round || !held(0))return 14;
    }
    if(wakeCallback->wakeups.load()!=0)return 15;
    printf("API32_SUSPEND_CONTROL_PASS uid=%u remote=1 version=1 host_locks=0_1_0 callbacks=2_2 duplicate_release=idempotent native_pid_and_time=verified kernel_suspend=unsupported wakeups_fabricated=0\n",getuid());
    return 0;
}
