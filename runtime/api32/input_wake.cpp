// SPDX-License-Identifier: GPL-3.0-only
// A real host PARTIAL_WAKE_LOCK replaces privileged EPOLLWAKEUP. The lease is
// owned through a remote HIDL Binder, so client death also drops the reference.
#include "input_wake.h"
#include "boot_hardware.h"
#include <android/system/suspend/1.0/ISystemSuspend.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <map>
#include <mutex>
#include <stdio.h>
#include <string.h>
using android::sp;
using namespace android::system::suspend::V1_0;
namespace {
struct Leases {
    std::mutex mutex;
    std::map<const void*,sp<IWakeLock>> owners;
};
Leases& leases() {static auto* value=new Leases;return *value;}
}
bool pocketInputAcquireWake32(const void* owner) {
    char sdk[PROP_VALUE_MAX];
    if(!owner || syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL ||
       __system_property_get("ro.build.version.sdk",sdk)<=0 || strcmp(sdk,"32"))return false;
    auto base=android::hardware::pocketRemoteHardwareService(ISystemSuspend::descriptor,"default");
    auto service=ISystemSuspend::castFrom(base).withDefault(nullptr);
    if(!service || !service->isRemote())return false;
    auto requested=service->acquireWakeLock(WakeLockType::PARTIAL,"Pocket12LInput");
    if(!requested.isOk())return false;
    sp<IWakeLock> lock=requested.withDefault(nullptr);
    if(!lock || !lock->isRemote())return false;
    auto& state=leases();bool inserted;
    {std::lock_guard<std::mutex> guard(state.mutex);inserted=state.owners.emplace(owner,lock).second;}
    if(!inserted){lock->release();return false;}
    fprintf(stderr,"API32_INPUT_WAKE_ACQUIRED host_partial=held kernel_capability=unchanged pid=%d\n",getpid());
    return true;
}
void pocketInputReleaseWake32(const void* owner) {
    auto& state=leases();sp<IWakeLock> lock;
    {
        std::lock_guard<std::mutex> guard(state.mutex);
        auto entry=state.owners.find(owner);if(entry==state.owners.end())return;
        lock=entry->second;state.owners.erase(entry);
    }
    auto result=lock->release();
    fprintf(stderr,"API32_INPUT_WAKE_RELEASED status=%s pid=%d\n",result.isOk()?"ok":"failed",getpid());
}
