// SPDX-License-Identifier: GPL-3.0-only
#include <binder/IServiceManager.h>
#include <hidl/ServiceManagement.h>
#include <hidl/HidlTransportSupport.h>
#include <hwbinder/ProcessState.h>
#include <android/hidl/manager/1.2/IServiceManager.h>
#include <android/hidl/manager/1.0/IServiceNotification.h>
#include <utils/String8.h>
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/system_properties.h>
#include <unistd.h>
using namespace android;
using Notification=hidl::manager::V1_0::IServiceNotification;
static bool memoryProfile(const sp<IServiceManager>& manager) {
    char value[PROP_VALUE_MAX];__system_property_get("ro.pocket.max_cached_processes",value);
    int limit=atoi(value);if(limit<2 || limit>32)return false;
    auto activity=manager->checkService(String16("activity"));if(!activity)return false;
    char path[]="/tmp/pocket-cache-settings-XXXXXX";int fd=mkstemp(path);if(fd<0)return false;
    if(unlink(path)){close(fd);return false;}
    FILE* file=fdopen(fd,"w+");if(!file){close(fd);return false;}
    Vector<String16> args;args.add(String16("settings"));
    bool ok=activity->dump(fd,args)==OK && !fseek(file,0,SEEK_SET);
    char bytes[262145];size_t count=ok?fread(bytes,1,sizeof(bytes),file):0;
    ok=ok && !ferror(file) && count<sizeof(bytes);fclose(file);
    std::string dump(bytes,count);
    ok=ok && dump.find("Permission Denial:")==std::string::npos &&
        dump.find("mOverrideMaxCachedProcesses=")==std::string::npos &&
        dump.find("\n  mCustomizedMaxCachedProcesses="+std::to_string(limit)+"\n")!=std::string::npos &&
        dump.find("\n  CUR_MAX_CACHED_PROCESSES="+std::to_string(limit)+"\n")!=std::string::npos;
    if(ok)printf("API32_GUEST_MEMORY_EARLY_DEFAULT_PASS constructor_limit=%d current_limit=%d override=-1 source=stock_ActivityManager_settings\n",limit,limit);
    else fprintf(stderr,"API32_GUEST_MEMORY_PROFILE_FAILED property=%s dump=%s\n",value,dump.c_str());
    return ok;
}
class Registration final : public Notification {
public:
    std::atomic<unsigned> calls{0};std::atomic<bool> invalid{false},preexisting{false};
    hardware::Return<void> onRegistration(const hardware::hidl_string& name,const hardware::hidl_string& instance,bool present) override {
        if(name!="android.hardware.media.c2@1.2::IComponentStore" || instance!="software")invalid=true;
        preexisting=present;++calls;return hardware::Void();
    }
};
int main() {
    setbuf(stdout,nullptr);if(getuid()!=0)return 64;
    hardware::configureRpcThreadpool(2,false);hardware::ProcessState::self()->startThreadPool();
    auto binder=defaultServiceManager();auto manager=hardware::defaultServiceManager1_2();
    if(!binder || !manager)return 1;
    if(!memoryProfile(binder))return 17;
    unsigned cold=0;bool listed=false;
    auto list=manager->list([&](const auto& names){for(const auto& name:names)if(name=="android.hardware.media.c2@1.2::IComponentStore/software")listed=true;});
    if(!list.isOk())return 3;
    cold+=!listed;sp<Registration> callback=new Registration;
    auto registered=manager->registerForNotifications("android.hardware.media.c2@1.2::IComponentStore","software",callback);
    if(!registered.isOk() || !registered.withDefault(false))return 7;
    registered=manager->registerForNotifications("android.hardware.media.c2@1.2::IComponentStore","software",callback);
    if(!registered.isOk() || !registered.withDefault(false))return 8;
    // This is the stock implementation used by generated IFoo::getService;
    // it must receive a registration callback for an initially absent service.
    auto store=hardware::details::getRawServiceInternal("android.hardware.media.c2@1.2::IComponentStore","software",true,false);
    if(!store)return 6;
    bool chain=false;
    auto status=store->interfaceChain([&](const auto& names){chain=names.size()==4 && names[0]=="android.hardware.media.c2@1.2::IComponentStore" && names[1]=="android.hardware.media.c2@1.1::IComponentStore" && names[2]=="android.hardware.media.c2@1.0::IComponentStore" && names[3]=="android.hidl.base@1.0::IBase";});
    if(!status.isOk() || !chain || !store->isRemote() || !store->ping().isOk())return 5;
    for(unsigned attempt=0;attempt<50 && !callback->calls;attempt++)usleep(100000);
    if(callback->calls!=1 || callback->invalid)return 9;
    auto removed=manager->unregisterForNotifications("android.hardware.media.c2@1.2::IComponentStore","software",callback);
    if(!removed.isOk() || !removed.withDefault(false))return 10;
    removed=manager->unregisterForNotifications("android.hardware.media.c2@1.2::IComponentStore","software",callback);
    if(!removed.isOk() || removed.withDefault(true))return 11;
    sp<Registration> warm=new Registration;
    registered=manager->registerForNotifications("android.hardware.media.c2@1.2::IComponentStore","software",warm);
    if(!registered.isOk() || !registered.withDefault(false))return 13;
    for(unsigned attempt=0;attempt<50 && !warm->calls;attempt++)usleep(100000);
    if(warm->calls!=1 || warm->invalid || !warm->preexisting)return 14;
    removed=manager->unregisterForNotifications("android.hardware.media.c2@1.2::IComponentStore","software",warm);
    if(!removed.isOk() || !removed.withDefault(false))return 15;
    if(callback->calls!=1)return 16;
    printf("API32_LAZY_HIDL_NOTIFICATIONS_PASS cold=%d preexisting=%d deduplicated=1 unregistered=1 warm_callback=1 stock_waiter=1\n",!listed,callback->preexisting.load());
    for(const char* name:{"media.extractor","media.player","media.metrics"}) {
        bool absent=!binder->checkService(String16(name));cold+=absent;
        printf("API32_LAZY_BINDER_REQUEST name=%s cold=%d\n",name,absent);
        auto service=binder->getService(String16(name));
        if(!service || service->pingBinder()!=OK){printf("API32_LAZY_BINDER_FAILED name=%s present=%d\n",name,service!=nullptr);return 2;}
        printf("API32_LAZY_BINDER_READY name=%s cold=%d descriptor=%s\n",name,absent,String8(service->getInterfaceDescriptor()).c_str());
    }
    if(!cold)return 12;
    printf("API32_LAZY_MEDIA_PASS native_binder_get=1 hardware_get=1 notifications=1 cold_services=%u\n",cold);return 0;
}
