// SPDX-License-Identifier: GPL-3.0-only
// Private combined native/HIDL registry on the ordinary app Binder driver.
// Host-generated templates bootstrap only this app's opaque Binder endpoint.
#include "boot_hardware.h"
#include "boot_health.h"
#include <hwbinder/Binder.h>
#include <hwbinder/IPCThreadState.h>
#include <hwbinder/Parcel.h>
#include <hwbinder/ProcessState.h>
#include <hidl/HidlBinderSupport.h>
#include <android/hidl/base/1.0/BpHwBase.h>
#include <android/hidl/base/1.0/BnHwBase.h>
#include <android/hardware/health/2.0/BnHwHealth.h>
#include <android/hidl/manager/1.1/BnHwServiceManager.h>
#include <utils/String8.h>
#include <map>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
namespace android { namespace hardware {
namespace {
bool readAll(FILE* stream, void* data, size_t size) {
    return fread(data, 1, size, stream) == size;
}

status_t loadTemplate(const char* path, Parcel& parcel, const sp<IBinder>& binder) {
    FILE* stream = fopen(path, "rb");
    if (!stream) return -errno;
    uint64_t sizes[2] = {};
    if (!readAll(stream, sizes, sizeof(sizes)) || sizes[0] > 65536 || sizes[0] % 4 || sizes[1] > 1) {
        fclose(stream);
        return BAD_VALUE;
    }
    const size_t length = sizes[0] + sizes[1] * sizeof(binder_size_t);
    auto* bytes = static_cast<uint8_t*>(malloc(length));
    if (!bytes) { fclose(stream); return NO_MEMORY; }
    bool valid = readAll(stream, bytes, length) && fgetc(stream) == EOF;
    fclose(stream);
    // Parcel data is 4-byte aligned; the separately stored offsets require
    // 8-byte alignment on ARM64. Copy instead of casting an unaligned pointer.
    binder_size_t offset = 0;
    if (sizes[1]) {
        memcpy(&offset, bytes + sizes[0], sizeof(offset));
        if (offset % 4 || sizes[0] < sizeof(flat_binder_object) || offset > sizes[0] - sizeof(flat_binder_object))
            valid = false;
        if (valid) {
            flat_binder_object object;
            memcpy(&object, bytes + offset, sizeof(object));
            if (!binder || !binder->localBinder() || object.hdr.type != BINDER_TYPE_BINDER)
                valid = false;
        }
    } else if (binder) valid = false;
    if (!valid) { free(bytes); return BAD_VALUE; }
    // Rebuild through the public Parcel API so it owns the Binder reference.
    // The host's bytes after the flat object include any newer stability trailer.
    status_t result;
    if (sizes[1]) {
        result = parcel.write(bytes, offset);
        if (result == NO_ERROR) result = parcel.writeStrongBinder(binder);
        const size_t end = offset + sizeof(flat_binder_object);
        if (result == NO_ERROR) result = parcel.write(bytes + end, sizes[0] - end);
    } else result = parcel.setData(bytes, sizes[0]);
    free(bytes);
    if (result != NO_ERROR) return result;
    return NO_ERROR;
}

int readNumber(const char* path) {
    FILE* stream = fopen(path, "r");
    if (!stream) return -1;
    int number = -1;
    if (fscanf(stream, "%d", &number) != 1) number = -1;
    fclose(stream);
    return number;
}

sp<IBinder> hostActivity() {
    Parcel request, reply;
    if (loadTemplate("/activityServiceRequest", request, nullptr) != NO_ERROR) return nullptr;
    // Bootstrap against host handle zero explicitly, avoiding the guest
    // getContextObject interposer and its recursion into itself.
    status_t result = IPCThreadState::self()->transact(0, 1, request, &reply, 0);
    if (result != NO_ERROR) return nullptr;
    int hostApi = readNumber("/host-api");
    if (hostApi < 28) return nullptr;
    // ServiceManager became AIDL in Android 11. Older native SM replies begin
    // directly with the Binder object, without an AIDL exception header.
    if (hostApi >= 30 && reply.readInt32() != 0) return nullptr;
    return reply.readStrongBinder(); // Any newer host stability trailer is last.
}

status_t broadcast(const sp<IBinder>& activity, const char* path, const sp<IBinder>& object) {
    int transaction = readNumber("/trans_code");
    if (!activity || transaction <= 0) return BAD_VALUE;
    Parcel request, reply;
    status_t result = loadTemplate(path, request, object);
    if (result != NO_ERROR) return result;
    result = activity->transact(transaction, request, &reply, 0);
    if (result != NO_ERROR) return result;
    return reply.readInt32();
}

class Receiver final : public BHwBinder {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    sp<IBinder> manager;
public:
    ~Receiver() override { pthread_cond_destroy(&condition); pthread_mutex_destroy(&mutex); }
    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags, TransactCallback callback) override {
        if (code != 2) return BHwBinder::onTransact(code, data, reply, flags, callback);
        int position = readNumber("/host-token-position");
        if (position < 0 || static_cast<size_t>(position) >= data.dataSize()) return BAD_VALUE;
        data.setDataPosition(position);
        if (data.readString16() != String16("com.github.ananbox.ILocalInterface")) return PERMISSION_DENIED;
        sp<IBinder> value = data.readStrongBinder();
        if (!value) return BAD_VALUE;
        pthread_mutex_lock(&mutex);
        manager = value;
        pthread_cond_signal(&condition);
        pthread_mutex_unlock(&mutex);
        if (reply) { reply->writeInt32(0); if (callback) callback(*reply); }
        return NO_ERROR;
    }
    sp<IBinder> wait() {
        timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        ++deadline.tv_sec;
        pthread_mutex_lock(&mutex);
        while (!manager) {
            int result = pthread_cond_timedwait(&condition, &mutex, &deadline);
            if (result != 0) break;
        }
        sp<IBinder> value = manager;
        pthread_mutex_unlock(&mutex);
        return value;
    }
};
}

namespace {
constexpr uint32_t kLookupHardware=0x10001;
constexpr uint32_t kNativePing=0x5f504e47;
constexpr const char* kHardwareToken="dev.kreijstal.pocket.HardwareRegistry";
using HardwareManager=::android::hidl::manager::V1_1::IServiceManager;
using HardwareNotification=::android::hidl::manager::V1_0::IServiceNotification;
class HardwareServices final : public HardwareManager {
    bool known(const hidl_string& descriptor,const hidl_string& name) {
        return descriptor=="android.hardware.health@2.0::IHealth" && (name=="default" || name=="backup");
    }
public:
    Return<sp<hidl::base::V1_0::IBase>> get(const hidl_string& descriptor,const hidl_string& name) override {
        sp<hidl::base::V1_0::IBase> service;
        if(known(descriptor,name)) service=pocketHealthRelay();
        return service;
    }
    // This profile has a fixed set of explicitly implemented services.
    Return<bool> add(const hidl_string&,const sp<hidl::base::V1_0::IBase>&) override {return false;}
    Return<Transport> getTransport(const hidl_string& descriptor,const hidl_string& name) override {
        return known(descriptor,name)?Transport::HWBINDER:Transport::EMPTY;
    }
    Return<void> list(list_cb cb) override {cb({"android.hardware.health@2.0::IHealth/default","android.hardware.health@2.0::IHealth/backup"});return Void();}
    Return<void> listByInterface(const hidl_string& descriptor,listByInterface_cb cb) override {
        if(descriptor=="android.hardware.health@2.0::IHealth") cb({"default","backup"});else cb({});
        return Void();
    }
    Return<bool> registerForNotifications(const hidl_string& descriptor,const hidl_string& name,const sp<HardwareNotification>& callback) override {
        if(!callback || descriptor!="android.hardware.health@2.0::IHealth") return false;
        bool ok=true;
        for(const char* instance:{"default","backup"}) if(name.empty() || name==instance) {
            auto result=callback->onRegistration(descriptor,instance,true);
            ok=ok && result.isOk();
        }
        fprintf(stderr,"DIRECT_HWB_NOTIFICATIONS descriptor=%s instance=%s callback_ok=%d\n",descriptor.c_str(),name.c_str(),ok);
        return ok;
    }
    Return<void> debugDump(debugDump_cb cb) override {cb({});return Void();}
    Return<void> registerPassthroughClient(const hidl_string&,const hidl_string&) override {return Void();}
    Return<bool> unregisterForNotifications(const hidl_string& descriptor,const hidl_string&,const sp<HardwareNotification>& callback) override {
        return callback!=nullptr && descriptor=="android.hardware.health@2.0::IHealth";
    }
};
class Registry final : public BHwBinder {
    struct Entry { sp<IBinder> binder; sp<IBinder::DeathRecipient> death; };
    std::map<String16,Entry> services;
    Mutex mutex;
    sp<IBinder> health;
    sp<IBinder> hardwareManager;
    class Death final : public IBinder::DeathRecipient {
        wp<Registry> owner;String16 name;
    public:
        Death(Registry* r,const String16& n):owner(r),name(n) {}
        void binderDied(const wp<IBinder>&) override {auto r=owner.promote();if(r) r->remove(name,this);}
    };
    void remove(const String16& name,const IBinder::DeathRecipient* death) {
        Mutex::Autolock lock(mutex);auto i=services.find(name);
        if(i!=services.end() && i->second.death.get()==death) services.erase(i);
    }
    status_t onTransact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags,TransactCallback cb) override {
        if(code==kNativePing) {if(cb && reply) cb(*reply);return NO_ERROR;}
        if(!reply || IPCThreadState::self()->getCallingUid()!=static_cast<uid_t>(syscall(SYS_getuid))) return PERMISSION_DENIED;
        if(code==kLookupHardware) {
            if(!data.enforceInterface(kHardwareToken)) return PERMISSION_DENIED;
            String16 descriptor=data.readString16(),instance=data.readString16();
            sp<IBinder> service;
            if(descriptor==String16("android.hardware.health@2.0::IHealth") && (instance==String16("default") || instance==String16("backup"))) service=health;
            if((descriptor==String16("android.hidl.manager@1.0::IServiceManager") || descriptor==String16("android.hidl.manager@1.1::IServiceManager")) && instance==String16("default")) service=hardwareManager;
            reply->writeStrongBinder(service);if(cb) cb(*reply);return NO_ERROR;
        }
        if(code<1 || code>4 || data.dataSize()<16) return UNKNOWN_TRANSACTION;
        // Native API 28 guest interface token: UID/PID, strict policy, UTF-16.
        data.readInt64();data.readInt32();
        if(data.readString16()!=String16("android.os.IServiceManager")) return PERMISSION_DENIED;
        {
        Mutex::Autolock lock(mutex);
        if(code==1 || code==2) {
            auto i=services.find(data.readString16());reply->writeStrongBinder(i==services.end()?nullptr:i->second.binder);
        } else if(code==3) {
            String16 name=data.readString16();auto service=data.readStrongBinder();
            if(!service || !name.size()) return BAD_VALUE;
            sp<IBinder::DeathRecipient> death=new Death(this,name);
            status_t result=service->linkToDeath(death);if(result!=NO_ERROR && result!=INVALID_OPERATION)return result;
            auto old=services.find(name);if(old!=services.end())old->second.binder->unlinkToDeath(old->second.death);
            services[name]={service,death};reply->writeInt32(0);
        } else {
            int index=data.readInt32();if(index<0 || static_cast<size_t>(index)>=services.size()) return BAD_VALUE;
            auto i=services.begin();std::advance(i,index);reply->writeString16(i->first);
        }
        }
        if(cb)cb(*reply);return NO_ERROR;
    }
public:
    Registry() {
        health=new health::V2_0::BnHwHealth(pocketHealthRelay());
        hardwareManager=new ::android::hidl::manager::V1_1::BnHwServiceManager(new HardwareServices);
    }
};
sp<IBinder> findRegistry() {
    sp<IBinder> activity=hostActivity();if(!activity)return nullptr;
    sp<Receiver> receiver=new Receiver;ProcessState::self()->startThreadPool();
    for(int i=0;i<15;++i) {
        if(broadcast(activity,"/localBroadcastIntent",receiver)!=NO_ERROR)return nullptr;
        auto manager=receiver->wait();if(manager)return manager;
    }
    return nullptr;
}
}
sp<hidl::base::V1_0::IBase> pocketRemoteHardwareService(const std::string& descriptor,const std::string& instance) {
    static Mutex mutex;static sp<IBinder> manager;
    Mutex::Autolock lock(mutex);
    if(!manager) manager=findRegistry();if(!manager)return nullptr;
    Parcel request,reply;request.writeInterfaceToken(kHardwareToken);
    request.writeString16(String16(descriptor.c_str()));request.writeString16(String16(instance.c_str()));
    status_t status=manager->transact(kLookupHardware,request,&reply,0);
    if(status!=NO_ERROR){manager.clear();return nullptr;}
    auto binder=reply.readStrongBinder();
    if(!binder)return nullptr;
    auto service=fromBinder<hidl::base::V1_0::IBase,hidl::base::V1_0::BpHwBase,hidl::base::V1_0::BnHwBase>(binder);
    fprintf(stderr,"DIRECT_HWB_SERVICE descriptor=%s remote=%d client_pid=%d\n",descriptor.c_str(),service && service->isRemote(),getpid());
    return service;
}
sp<hidl::manager::V1_0::IServiceManager> defaultServiceManager() {
    auto base=pocketRemoteHardwareService(HardwareManager::descriptor,"default");
    if(!base)return nullptr;
    return HardwareManager::castFrom(base).withDefault(nullptr);
}
// No request may fall back to a host hardware/global context manager.
sp<IBinder> ProcessState::getContextObject(const sp<IBinder>&) {return nullptr;}
sp<IBinder> ProcessState::getContextObject(const String16&,const sp<IBinder>&) {return nullptr;}
bool ProcessState::becomeContextManager(context_check_func,void*) {errno=ENOTSUP;return false;}
}} // android::hardware
extern "C" int pocketStartHardwareServices() {
    using namespace android;using namespace android::hardware;
    setbuf(stdout,nullptr);if(getuid()!=0)return 64;
    ProcessState::self()->startThreadPool();sp<Registry> registry=new Registry;
    status_t result=broadcast(hostActivity(),"/binderBroadcastIntent",registry);
    fprintf(stderr,"DIRECT_HWB_REGISTRY_READY status=%d pid=%d kernel_uid=%ld\n",result,getpid(),syscall(SYS_getuid));
    if(result!=NO_ERROR)return 1;
    IPCThreadState::self()->joinThreadPool();return 0;
}
