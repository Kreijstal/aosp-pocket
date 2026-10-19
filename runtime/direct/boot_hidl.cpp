// SPDX-License-Identifier: GPL-3.0-only
// Explicit in-process software-graphics HAL profile. Never uses host hwbinder.
#include <android/hidl/base/1.0/IBase.h>
#include <dlfcn.h>
#include "SurfaceFlingerConfigs.h"
#include "boot_hardware.h"
#include <stdio.h>
#include <string>
#include <utils/StrongPointer.h>
namespace android { namespace hardware { namespace details {
sp<::android::hidl::base::V1_0::IBase> getRawServiceInternal(
        const std::string& descriptor,const std::string& instance,bool retry,bool getStub) {
    (void)retry; (void)getStub;
    using ::android::hidl::base::V1_0::IBase;
    if ((descriptor=="android.hardware.configstore@1.0::ISurfaceFlingerConfigs" ||
         descriptor=="android.hardware.configstore@1.1::ISurfaceFlingerConfigs") && instance=="default") {
        fprintf(stderr,"DIRECT_HAL_CONFIGSTORE %s/default source=android-9.0.0_r61\n",descriptor.c_str());
        return new ::android::hardware::configstore::V1_1::implementation::SurfaceFlingerConfigs();
    }
    if ((descriptor=="android.hardware.health@2.0::IHealth" && (instance=="default" || instance=="backup")) ||
        ((descriptor=="android.hidl.manager@1.0::IServiceManager" || descriptor=="android.hidl.manager@1.1::IServiceManager") && instance=="default")) return pocketRemoteHardwareService(descriptor,instance);
    if(descriptor=="android.hardware.keymaster@3.0::IKeymasterDevice" && (instance=="default" || instance=="softwareonly")) {
        void* handle=dlopen("/vendor/lib64/hw/android.hardware.keymaster@3.0-impl.so",RTLD_NOW|RTLD_LOCAL);
        if(!handle) {fprintf(stderr,"DIRECT_KEYMASTER_LOAD_FAILED %s\n",dlerror());return nullptr;}
        using Fetch=IBase*(*)(const char*);
        auto fetch=reinterpret_cast<Fetch>(dlsym(handle,"HIDL_FETCH_IKeymasterDevice"));
        sp<IBase> service=fetch?fetch("softwareonly"):nullptr;
        fprintf(stderr,"DIRECT_HAL_SOFTWARE_KEYMASTER available=%d hardware_device=unused\n",service!=nullptr);
        return service;
    }
    // Retain a precise profile; unsupported HALs remain unavailable.
    if (descriptor!="android.hardware.graphics.composer@2.1::IComposer" &&
        descriptor!="android.hardware.graphics.allocator@2.0::IAllocator" &&
        descriptor!="android.hardware.graphics.mapper@2.0::IMapper") {
        fprintf(stderr,"DIRECT_HAL_UNAVAILABLE %s/%s\n",descriptor.c_str(),instance.c_str());
        return nullptr;
    }
    size_t separator=descriptor.find("::");
    std::string library="/vendor/lib64/hw/"+descriptor.substr(0,separator)+"-impl.so";
    void* handle=dlopen(library.c_str(),RTLD_NOW|RTLD_LOCAL);
    if (!handle) { fprintf(stderr,"DIRECT_HAL_LOAD_FAILED %s: %s\n",library.c_str(),dlerror());return nullptr; }
    using Fetch=IBase*(*)(const char*);
    Fetch fetch=reinterpret_cast<Fetch>(dlsym(handle,("HIDL_FETCH_"+descriptor.substr(separator+2)).c_str()));
    if (!fetch) { fprintf(stderr,"DIRECT_HAL_FETCH_MISSING %s\n",library.c_str());dlclose(handle);return nullptr; }
    sp<IBase> service=fetch(instance.c_str());
    fprintf(stderr,"DIRECT_HAL_PASSTHROUGH %s/%s available=%d\n",descriptor.c_str(),instance.c_str(),service!=nullptr);
    // Keep the implementation loaded while its vtables/threads may be live.
    return service;
}
}}}
