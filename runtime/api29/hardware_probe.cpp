// SPDX-License-Identifier: GPL-3.0-only
#include <hidl/ServiceManagement.h>
#include <android/hidl/manager/1.2/IServiceManager.h>
#include <android/hardware/health/2.0/IHealth.h>
#include <stdio.h>
#include <unistd.h>
#include "power.h"
using namespace android;
using namespace android::hardware;
int main() {
    setbuf(stdout,nullptr);
    auto manager=defaultServiceManager1_2();
    if(!manager || !manager->isRemote())return 1;
    bool listed=false;
    auto result=manager->listManifestByInterface("android.hardware.health@2.0::IHealth",[&](const auto& names){
        listed=names.size()==2 && names[0]=="backup" && names[1]=="default";
    });
    if(!result.isOk() || !listed)return 2;
    bool empty=false;
    result=manager->listManifestByInterface("dev.kreijstal.pocket.Unavailable",[&](const auto& names){empty=names.size()==0;});
    if(!result.isOk() || !empty)return 3;
    auto health=health::V2_0::IHealth::getService("default");
    if(!health || !health->isRemote())return 4;
    int server=0;
    result=health->getDebugInfo([&](const auto& info){server=info.pid;});
    if(!result.isOk() || server<=1 || server==getpid())return 5;
    auto suspend=android::system::suspend::V1_0::ISystemSuspend::getService("default");
    if(!suspend || !suspend->isRemote())return 6;
    uint64_t count=0;
    if(pocketPowerRequest(3,0,nullptr,&count) || count!=0)return 7;
    sp<android::system::suspend::V1_0::IWakeLock> lock=suspend->acquireWakeLock(
        android::system::suspend::V1_0::WakeLockType::PARTIAL,"Api29HardwareProbe");
    if(!lock || pocketPowerRequest(3,0,nullptr,&count) || count!=1)return 8;
    if(!lock->release().isOk())return 9;
    for(unsigned n=0;n<100;n++) {
        if(pocketPowerRequest(3,0,nullptr,&count))return 10;
        if(count==0)break;
        usleep(20000);
    }
    if(count!=0 || !lock->release().isOk())return 11;
    puts("API29_HOST_WAKE_LOCK_PASS acquire=held release=unheld duplicate_release=harmless");
    printf("API29_HARDWARE_PASS manager=1.2 remote=1 manifest_sorted=1 absent_service_empty=1 server_pid=%d client_pid=%d\n",server,getpid());
    return 0;
}
