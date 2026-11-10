// SPDX-License-Identifier: GPL-3.0-only
#include <hidl/ServiceManagement.h>
#include <android/hidl/manager/1.2/IServiceManager.h>
#include <android/hardware/health/2.0/IHealth.h>
#include "boot_hardware.h"
#include <stdio.h>
#include <unistd.h>
#include <atomic>
#include <string.h>
#include "../api29/power.h"
using namespace android;
using namespace android::hardware;
class ChainMismatchProbe32 final : public hidl::base::V1_0::IBase {
public:
    std::atomic<unsigned> calls{0};
    Return<void> interfaceChain(interfaceChain_cb callback) override {
        ++calls; callback({hidl::base::V1_0::IBase::descriptor}); return Void();
    }
};
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);
    bool cameraDenied=argc==2 && !strcmp(argv[1],"camera-role-denied");
    bool denied=cameraDenied || (argc==2 && !strcmp(argv[1],"denied"));
    bool framework=argc==2 && !strcmp(argv[1],"framework");
    bool camera=argc==2 && !strcmp(argv[1],"camera");
    if((argc!=1 && !denied && !framework && !camera) || getuid()!=0)return 64;
    if(denied && (setgid(cameraDenied?1047:39999) || setuid(cameraDenied?1047:39999)))return 64;
    if((framework || camera) && (setgid(1000) || setuid(1000)))return 64;
    auto manager=defaultServiceManager1_2();
    if(!manager || !manager->isRemote())return 1;
    if(camera) {
        const char* descriptors[]={"android.frameworks.cameraservice.service@2.2::ICameraService",
            "android.frameworks.cameraservice.service@2.1::ICameraService",
            "android.frameworks.cameraservice.service@2.0::ICameraService"};
        for(unsigned round=0;round<600;++round) {
            bool all=true;
            for(const char* descriptor:descriptors) {
                auto result=manager->get(descriptor,"default");if(!result.isOk())return 30;
                auto service=result.withDefault(nullptr);if(!service){all=false;break;}
                bool matches=false;
                auto chain=service->interfaceChain([&](const auto& actual){
                    matches=actual.size()==4 && actual[0]==descriptors[0] && actual[1]==descriptors[1] &&
                        actual[2]==descriptors[2] && actual[3]==hidl::base::V1_0::IBase::descriptor;
                });
                if(!service->isRemote() || !chain.isOk() || !matches || !service->ping().isOk())return 31;
            }
            if(all){puts("API32_CAMERA_HIDL_PASS stock_service=2.2 aliases=2.1,2.0 chain=verified remote=1 ping=1");return 0;}
            usleep(100000);
        }
        puts("API32_CAMERA_HIDL_TIMEOUT");return 32;
    }
    if(framework) {
        const char* descriptors[]={"android.frameworks.displayservice@1.0::IDisplayService",
            "android.frameworks.stats@1.0::IStats", "android.frameworks.sensorservice@1.0::ISensorManager",
            "android.frameworks.schedulerservice@1.0::ISchedulingPolicyService"};
        bool verified[4]={};
        for(unsigned round=0;round<900;++round) {
            bool all=true;
            for(unsigned i=0;i<4;++i) {
                if(verified[i])continue;
                auto result=manager->get(descriptors[i],"default");
                if(!result.isOk())return 22;
                auto managed=result.withDefault(nullptr);
                if(!managed){all=false;continue;}
                auto direct=pocketRemoteHardwareService(descriptors[i],"default");
                if(!managed->isRemote() || !direct || !direct->isRemote())return 23;
                for(const auto& service:{managed,direct}) {
                    bool matches=false;
                    auto chain=service->interfaceChain([&](const auto& actual){
                        matches=actual.size()==2 && actual[0]==descriptors[i] &&
                            actual[1]==hidl::base::V1_0::IBase::descriptor;
                    });
                    if(!chain.isOk() || !matches || !service->ping().isOk())return 24;
                }
                verified[i]=true;
                printf("API32_FRAMEWORK_HIDL_CLIENT descriptor=%s remote=1 manager_and_direct=1\n",descriptors[i]);
            }
            if(all){puts("API32_FRAMEWORK_HIDL_CLIENT_PASS services=4 remote=1 interface_chains=verified ping=1");return 0;}
            usleep(100000);
        }
        puts("API32_FRAMEWORK_HIDL_CLIENT_TIMEOUT");return 25;
    }
    if(denied) {
        sp<ChainMismatchProbe32> negative=new ChainMismatchProbe32;
        auto result=manager->addWithChain("default",negative,
            {"android.frameworks.sensorservice@1.0::ISensorManager",hidl::base::V1_0::IBase::descriptor});
        if(!result.isOk() || result.withDefault(true) || negative->calls.load()!=0)return 21;
        puts(cameraDenied?"API32_CAMERA_HIDL_ROLE_DENIED_PASS uid=1047 unrelated_service=rejected before_interface_query=1":
            "API32_FRAMEWORK_HIDL_ROLE_DENIED_PASS uid=39999 rejected_before_interface_query=1");return 0;
    }
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
    using Transport=android::hidl::manager::V1_0::IServiceManager::Transport;
    for(const char* descriptor:{"android.frameworks.sensorservice@1.0::ISensorManager",
            "android.frameworks.displayservice@1.0::IDisplayService",
            "android.frameworks.schedulerservice@1.0::ISchedulingPolicyService",
            "android.frameworks.stats@1.0::IStats"}) {
        auto transport=manager->getTransport(descriptor,"default");
        if(!transport.isOk() || transport.withDefault(Transport::EMPTY)!=Transport::HWBINDER)return 15;
        auto missing=manager->get(descriptor,"default");
        if(!missing.isOk() || missing.withDefault(nullptr))return 16;
    }
    auto absent=manager->getTransport("dev.kreijstal.pocket.Unavailable","default");
    if(!absent.isOk() || absent.withDefault(Transport::HWBINDER)!=Transport::EMPTY)return 17;
    hidl_vec<hidl_string> healthChain;
    auto chainRead=health->interfaceChain([&](const auto& chain){healthChain=chain;});
    if(!chainRead.isOk() || healthChain.size()<2)return 18;
    auto fixed=manager->addWithChain("default",health,healthChain);
    if(!fixed.isOk() || fixed.withDefault(true))return 19;
    sp<ChainMismatchProbe32> negative=new ChainMismatchProbe32;
    auto mismatched=manager->addWithChain("default",negative,
        {"android.frameworks.sensorservice@1.0::ISensorManager",hidl::base::V1_0::IBase::descriptor});
    if(!mismatched.isOk() || mismatched.withDefault(true) || negative->calls.load()!=1)return 20;
    puts("API32_FRAMEWORK_HIDL_POLICY_PASS declarations=4 unknown_absent=1 mismatched_chain_rejected=1 fixed_service_rejected=1");
    int server=0;
    result=health->getDebugInfo([&](const auto& info){server=info.pid;});
    if(!result.isOk() || server<=1 || server==getpid())return 5;
    // Compare the HIDL health reply with the actual host BatteryManager relay.
    FILE* source=fopen("/pocket-host-battery","r");
    long long stamp=0;int present=0,plugged=0,status=0,state=0,level=0,scale=0;
    if(!source)return 12;
    int fields=fscanf(source,"POCKET_BATTERY_V1\n%lld %d %d %d %d %d %d",&stamp,&present,&plugged,&status,&state,&level,&scale);
    fclose(source);
    if(fields!=7 || scale<=0 || level<0 || level>scale)return 13;
    bool battery=false;
    auto reading=health->getHealthInfo([&](auto status,const auto& info){
        battery=status==health::V2_0::Result::SUCCESS && info.legacy.batteryPresent==bool(present) &&
            info.legacy.batteryLevel==100*level/scale;
    });
    if(!reading.isOk() || !battery)return 14;
    puts("API32_HOST_BATTERY_PASS source=BatteryManager remote=1");
    auto suspend=android::system::suspend::V1_0::ISystemSuspend::getService("default");
    if(!suspend || !suspend->isRemote())return 6;
    uint64_t count=0;
    if(pocketPowerRequest(3,0,nullptr,&count) || count!=0)return 7;
    sp<android::system::suspend::V1_0::IWakeLock> lock=suspend->acquireWakeLock(
        android::system::suspend::V1_0::WakeLockType::PARTIAL,"Api32HardwareProbe");
    if(!lock || pocketPowerRequest(3,0,nullptr,&count) || count!=1)return 8;
    if(!lock->release().isOk())return 9;
    for(unsigned n=0;n<100;n++) {
        if(pocketPowerRequest(3,0,nullptr,&count))return 10;
        if(count==0)break;
        usleep(20000);
    }
    if(count!=0 || !lock->release().isOk())return 11;
    puts("API32_HOST_WAKE_LOCK_PASS acquire=held release=unheld duplicate_release=harmless");
    printf("API32_HARDWARE_PASS manager=1.2 remote=1 manifest_sorted=1 absent_service_empty=1 server_pid=%d client_pid=%d\n",server,getpid());
    return 0;
}
