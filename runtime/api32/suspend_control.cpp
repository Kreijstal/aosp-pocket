// SPDX-License-Identifier: GPL-3.0-only
// 12L suspend control over Pocket's host-delegated partial wake locks.
#include "SuspendControlService.h"
#include "WakeLockEntryList.h"
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <stdio.h>
#include <unistd.h>

namespace {
using namespace android;
using namespace android::system::suspend;
using namespace android::system::suspend::internal;
using android::system::suspend::V1_0::WakeLockEntryList;

bool allowed() {
    const uid_t uid=IPCThreadState::self()->getCallingUid();
    return uid==0 || uid==1000;
}
binder::Status denied() {
    return binder::Status::fromExceptionCode(binder::Status::EX_SECURITY,
        "Suspend control requires the guest root or system role");
}
binder::Status unsupported() {
    return binder::Status::fromExceptionCode(binder::Status::EX_UNSUPPORTED_OPERATION,
        "Pocket cannot observe or control host kernel suspension");
}

class Control final : public V1_0::SuspendControlService {
public:
    binder::Status registerCallback(const sp<ISuspendCallback>& callback,bool* result) override {
        if(!allowed())return denied();
        return V1_0::SuspendControlService::registerCallback(callback,result);
    }
    binder::Status registerWakelockCallback(const sp<IWakelockCallback>& callback,
            const std::string& name,bool* result) override {
        if(!allowed())return denied();
        return V1_0::SuspendControlService::registerWakelockCallback(callback,name,result);
    }
};
sp<Control> control() {static sp<Control> service=new Control;return service;}
WakeLockEntryList& stats() {
    // Native acquisitions only. No kernel counters are fabricated or exposed.
    static WakeLockEntryList list(128,android::base::unique_fd(-1));
    return list;
}

class Internal final : public BnSuspendControlServiceInternal {
public:
    binder::Status enableAutosuspend(bool* result) override {
        if(!allowed())return denied();
        *result=false;return binder::Status::ok();
    }
    binder::Status forceSuspend(bool* result) override {
        if(!allowed())return denied();
        *result=false;return binder::Status::ok();
    }
    binder::Status getWakeLockStats(std::vector<WakeLockInfo>* result) override {
        if(!allowed())return denied();
        result->clear();stats().updateNow();stats().getWakeLockStats(result);
        return binder::Status::ok();
    }
    binder::Status getWakeupStats(std::vector<WakeupInfo>*) override {
        if(!allowed())return denied();
        return unsupported();
    }
    binder::Status getSuspendStats(SuspendInfo*) override {
        if(!allowed())return denied();
        return unsupported();
    }
    status_t dump(int fd,const Vector<String16>&) override {
        if(!allowed())return PERMISSION_DENIED;
        dprintf(fd,"Pocket native wake-lock stats; host kernel suspend controls/stats unavailable\n");
        std::vector<WakeLockInfo> entries;
        getWakeLockStats(&entries);
        for(const auto& entry:entries)
            dprintf(fd,"name=%s pid=%d active=%d count=%lld total_ms=%lld\n",entry.name.c_str(),
                entry.pid,entry.isActive,(long long)entry.activeCount,(long long)entry.totalTime);
        return NO_ERROR;
    }
};
}

void pocketSuspendWakeAcquired32(const std::string& name,pid_t pid) {
    stats().updateOnAcquire(name,pid,android::system::suspend::V1_0::getTimeNow());
    control()->notifyWakelock(name,true);
}
void pocketSuspendWakeReleased32(const std::string& name,pid_t pid) {
    stats().updateOnRelease(name,pid,android::system::suspend::V1_0::getTimeNow());
    control()->notifyWakelock(name,false);
}
extern "C" int pocketRegisterSuspendControl32() {
    android::ProcessState::self()->startThreadPool();
    auto manager=android::defaultServiceManager();
    if(!manager)return 1;
    auto status=manager->addService(android::String16("suspend_control"),control());
    if(status!=android::NO_ERROR)return 2;
    status=manager->addService(android::String16("suspend_control_internal"),new Internal);
    if(status!=android::NO_ERROR)return 3;
    printf("API32_SUSPEND_CONTROL_READY callbacks=stock_v1 stats=native_host_partial_locks kernel_suspend=unsupported pid=%d\n",getpid());
    return 0;
}
