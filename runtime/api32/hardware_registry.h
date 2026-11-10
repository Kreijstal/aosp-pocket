// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../api28/binder_identity.h"
#include "lazy_service_runtime.h"
#include <android/hidl/manager/1.0/IServiceNotification.h>
#include <hidl/HidlBinderSupport.h>
#include <algorithm>
#include <utils/Mutex.h>
#include <map>
#include <string>
#include <vector>

// Only declared stock framework services and enabled software HALs register here.
// Logical roles are guest metadata; the outer app UID remains the host boundary.
class FrameworkHardwareRegistry32 final : public android::RefBase {
    using Base = android::hidl::base::V1_0::IBase;
    using DeathBase = android::hardware::hidl_death_recipient;
    using Notification = android::hidl::manager::V1_0::IServiceNotification;
    struct Entry { android::sp<Base> service; android::sp<DeathBase> death; };
    struct Watch {uint64_t id;std::string descriptor,instance;android::sp<Notification> callback;android::sp<android::hardware::IBinder> binder;android::sp<DeathBase> death;};
    android::Mutex mutex;
    std::map<std::string, Entry> entries;
    std::vector<Watch> watches;
    uint64_t nextWatch=1;
    void forgetWatch(uint64_t id) {
        android::Mutex::Autolock lock(mutex);
        watches.erase(std::remove_if(watches.begin(),watches.end(),[&](const auto& value){return value.id==id;}),watches.end());
    }
    class WatchDeath final : public DeathBase {
        android::wp<FrameworkHardwareRegistry32> owner;uint64_t id;
    public:
        WatchDeath(FrameworkHardwareRegistry32* registry,uint64_t value):owner(registry),id(value){}
        void serviceDied(uint64_t,const android::wp<Base>&) override {auto registry=owner.promote();if(registry)registry->forgetWatch(id);}
    };
    class Death final : public DeathBase {
        android::wp<FrameworkHardwareRegistry32> registry;
        std::string descriptor;
    public:
        Death(FrameworkHardwareRegistry32* owner, const std::string& name)
            : registry(owner), descriptor(name) {}
        void serviceDied(uint64_t, const android::wp<Base>&) override {
            auto owner = registry.promote();
            if (!owner) return;
            android::Mutex::Autolock lock(owner->mutex);
            auto found = owner->entries.find(descriptor);
            if (found != owner->entries.end() && found->second.death.get() == this)
                owner->entries.erase(found);
        }
    };
public:
    static bool cameraService(const std::string& descriptor) {
        return descriptor=="android.frameworks.cameraservice.service@2.2::ICameraService" ||
            descriptor=="android.frameworks.cameraservice.service@2.1::ICameraService" ||
            descriptor=="android.frameworks.cameraservice.service@2.0::ICameraService";
    }
    // Stock mediaswcodec publishes the platform software Codec2 store as v1.2;
    // clients look it up through any interface in its chain.
    static bool codec2Store(const std::string& descriptor) {
        return descriptor=="android.hardware.media.c2@1.2::IComponentStore" ||
            descriptor=="android.hardware.media.c2@1.1::IComponentStore" ||
            descriptor=="android.hardware.media.c2@1.0::IComponentStore";
    }
    static bool declared(const std::string& descriptor, const std::string& instance) {
        if (instance == "software")
            return codec2Store(descriptor) && property_get_bool("ro.pocket.media.enabled",false);
        return instance == "default" &&
            (cameraService(descriptor) || descriptor == "android.frameworks.sensorservice@1.0::ISensorManager" ||
             descriptor == "android.frameworks.displayservice@1.0::IDisplayService" ||
             descriptor == "android.frameworks.schedulerservice@1.0::ISchedulingPolicyService" ||
             descriptor == "android.frameworks.stats@1.0::IStats" ||
             (property_get_bool("ro.pocket.keystore.enabled",false) &&
              (descriptor == "android.hardware.keymaster@4.1::IKeymasterDevice" ||
               descriptor == "android.hardware.keymaster@4.0::IKeymasterDevice")) ||
             (property_get_bool("ro.pocket.gatekeeper.enabled",false) &&
              descriptor == "android.hardware.gatekeeper@1.0::IGatekeeper"));
    }
    static bool mayRegister(const std::string& descriptor="") {
        auto* caller = android::hardware::IPCThreadState::self();
        const uid_t kernelUid = pocket_kernel_uid_from_proc();
        uint64_t identity = 0;
        return kernelUid != static_cast<uid_t>(-1) && caller->getCallingUid() == kernelUid &&
            pocket_binder_identity(caller->getCallingPid(), &identity) &&
            ((identity >> 32) == 0 || (identity >> 32) == 1000 ||
             ((identity >> 32) == 1047 && (descriptor.empty() ||
              descriptor=="android.frameworks.cameraservice.service@2.2::ICameraService")) ||
             // mediacodec (AID_MEDIA_CODEC) runs stock mediaswcodec.
             ((identity >> 32) == 1046 && (descriptor.empty() ||
              descriptor=="android.hardware.media.c2@1.2::IComponentStore")));
    }
    bool add(const android::hardware::hidl_string& instance, const android::sp<Base>& service,
             const android::hardware::hidl_vec<android::hardware::hidl_string>& claimed) {
        if (!service || !service->isRemote() || claimed.size() < 2 || !mayRegister(claimed[0].c_str()) ||
            !declared(claimed[0].c_str(), instance.c_str()))
            return false;
        const bool keymaster=claimed[0]=="android.hardware.keymaster@4.1::IKeymasterDevice";
        const bool camera=claimed[0]=="android.frameworks.cameraservice.service@2.2::ICameraService";
        const bool codec2=claimed[0]=="android.hardware.media.c2@1.2::IComponentStore";
        if(keymaster) {
            if(claimed.size()!=3 || claimed[1]!="android.hardware.keymaster@4.0::IKeymasterDevice" ||
               claimed[2]!=Base::descriptor)return false;
        } else if(camera) {
            if(claimed.size()!=4 || claimed[1]!="android.frameworks.cameraservice.service@2.1::ICameraService" ||
               claimed[2]!="android.frameworks.cameraservice.service@2.0::ICameraService" || claimed[3]!=Base::descriptor)return false;
        } else if(codec2) {
            if(claimed.size()!=4 || claimed[1]!="android.hardware.media.c2@1.1::IComponentStore" ||
               claimed[2]!="android.hardware.media.c2@1.0::IComponentStore" || claimed[3]!=Base::descriptor)return false;
        } else if(cameraService(claimed[0].c_str()) || codec2Store(claimed[0].c_str()) || claimed.size()!=2 || claimed[1]!=Base::descriptor ||
                  claimed[0]=="android.hardware.keymaster@4.0::IKeymasterDevice")return false;
        bool matches = false;
        auto chain = service->interfaceChain([&](const auto& actual) {
            matches = actual.size() == claimed.size();
            for (size_t i = 0; matches && i < actual.size(); ++i)
                matches = actual[i] == claimed[i];
        });
        if (!chain.isOk() || !matches) return false;
        std::string descriptor = claimed[0].c_str();
        android::sp<DeathBase> death = new Death(this, descriptor);
        // Serialize publication with death cleanup so a dead server cannot leave
        // an entry behind between linkToDeath and insertion.
        std::vector<Watch> notify;
        {
            android::Mutex::Autolock lock(mutex);
            auto linked=service->linkToDeath(death,0);
            if(!linked.isOk() || !static_cast<bool>(linked))return false;
            if(entries.count(descriptor)){service->unlinkToDeath(death);return false;}
            entries.emplace(descriptor,Entry{service,death});
            for(const auto& watch:watches) {
                if(!watch.instance.empty() && watch.instance!=instance.c_str())continue;
                for(const auto& name:claimed)if(watch.descriptor==name.c_str()){notify.push_back(watch);break;}
            }
        }
        for(const auto& watch:notify)if(!watch.callback->onRegistration(watch.descriptor,instance,false).isOk())forgetWatch(watch.id);
        fprintf(stderr, "API32_FRAMEWORK_HIDL_REGISTERED descriptor=%s instance=%s caller_pid=%d\n",
                descriptor.c_str(), instance.c_str(), android::hardware::IPCThreadState::self()->getCallingPid());
        return true;
    }
    bool subscribe(const std::string& descriptor,const std::string& instance,const android::sp<Notification>& callback) {
        std::string actual=instance.empty() && declared(descriptor,"software")?"software":instance.empty()?"default":instance;
        if(!callback || !callback->isRemote() || !declared(descriptor,actual))return false;
        auto binder=android::hardware::toBinder(callback);if(!binder)return false;
        const std::string canonical=cameraService(descriptor)?"android.frameworks.cameraservice.service@2.2::ICameraService":codec2Store(descriptor)?"android.hardware.media.c2@1.2::IComponentStore":descriptor=="android.hardware.keymaster@4.0::IKeymasterDevice"?"android.hardware.keymaster@4.1::IKeymasterDevice":descriptor;
        uint64_t id;bool present;
        {
            android::Mutex::Autolock lock(mutex);
            for(const auto& watch:watches)if(watch.descriptor==descriptor && watch.instance==instance && watch.binder==binder)return true;
            if(watches.size()>=128 || nextWatch==UINT64_MAX)return false;
            id=nextWatch++;android::sp<DeathBase> death=new WatchDeath(this,id);
            auto linked=callback->linkToDeath(death,id);if(!linked.isOk() || !static_cast<bool>(linked))return false;
            watches.push_back({id,descriptor,instance,callback,binder,death});present=entries.count(canonical)!=0;
        }
        if(present && !callback->onRegistration(descriptor,actual,true).isOk()){forgetWatch(id);return false;}
        return true;
    }
    bool unsubscribe(const std::string& descriptor,const std::string& instance,const android::sp<Notification>& callback) {
        if(!callback)return false;
        auto binder=android::hardware::toBinder(callback);if(!binder)return false;
        std::vector<Watch> removed;
        {
            android::Mutex::Autolock lock(mutex);
            for(auto i=watches.begin();i!=watches.end();) {
                if(i->descriptor==descriptor && i->instance==instance && i->binder==binder){removed.push_back(*i);i=watches.erase(i);}else ++i;
            }
        }
        for(const auto& watch:removed)watch.callback->unlinkToDeath(watch.death);
        return !removed.empty();
    }
    android::sp<Base> get(const std::string& descriptor, const std::string& instance) {
        if (!declared(descriptor, instance)) return nullptr;
        if(descriptor=="android.hardware.keymaster@4.1::IKeymasterDevice" ||
           descriptor=="android.hardware.keymaster@4.0::IKeymasterDevice" ||
           descriptor=="android.hardware.gatekeeper@1.0::IGatekeeper") {
            auto* caller=android::hardware::IPCThreadState::self();uint64_t identity=0;
            const uid_t kernelUid=pocket_kernel_uid_from_proc();
            if(kernelUid==static_cast<uid_t>(-1) || caller->getCallingUid()!=kernelUid ||
               !pocket_binder_identity(caller->getCallingPid(),&identity))return nullptr;
            const uint32_t uid=static_cast<uint32_t>(identity>>32);
            if(uid!=0 && uid!=1000 && uid!=1017)return nullptr;
            if(descriptor=="android.hardware.gatekeeper@1.0::IGatekeeper" && uid==1017)return nullptr;
        }
        const std::string canonical=cameraService(descriptor)?"android.frameworks.cameraservice.service@2.2::ICameraService":
            codec2Store(descriptor)?"android.hardware.media.c2@1.2::IComponentStore":
            descriptor=="android.hardware.keymaster@4.0::IKeymasterDevice"?"android.hardware.keymaster@4.1::IKeymasterDevice":descriptor;
        {
            android::Mutex::Autolock lock(mutex);
            auto found=entries.find(canonical);if(found!=entries.end())return found->second.service;
        }
        pocket::requestLazyService32((canonical+"/"+instance).c_str(),true);
        return nullptr;
    }
    std::vector<std::string> list() {
        android::Mutex::Autolock lock(mutex);
        std::vector<std::string> result;
        for (const auto& entry : entries) {
            if(entry.first=="android.hardware.media.c2@1.2::IComponentStore") {
                for(const char* version:{"1.2","1.1","1.0"})
                    result.push_back(std::string("android.hardware.media.c2@")+version+"::IComponentStore/software");
                continue;
            }
            result.push_back(entry.first + "/default");
            if(entry.first=="android.hardware.keymaster@4.1::IKeymasterDevice")
                result.push_back("android.hardware.keymaster@4.0::IKeymasterDevice/default");
            if(entry.first=="android.frameworks.cameraservice.service@2.2::ICameraService") {
                result.push_back("android.frameworks.cameraservice.service@2.1::ICameraService/default");
                result.push_back("android.frameworks.cameraservice.service@2.0::ICameraService/default");
            }
        }
        return result;
    }
};
