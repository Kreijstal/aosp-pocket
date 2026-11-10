// SPDX-License-Identifier: GPL-3.0-only
// Explicit 12L profile: private HIDL services, with no host manager fallback.
#include <hidl/ServiceManagement.h>
#include "boot_hardware.h"
#include "lazy_service_runtime.h"
#include <stdio.h>
#include <dlfcn.h>
#include <cutils/properties.h>

namespace android { namespace hardware { namespace details {
sp<::android::hidl::base::V1_0::IBase> getRawServiceInternal(
        const std::string& descriptor,const std::string& instance,bool retry,bool getStub) {
    const auto* lazy=pocket::lazyService32((descriptor+"/"+instance).c_str(),true);
    if(lazy && pocket::lazyMediaEnabled32()) {
        // Only declared remote services enter the stock wait/notification
        // path. Its manager entry points above remain Pocket-private.
        if(getStub || (lazy->requiresMedia && !property_get_bool("ro.pocket.media.enabled",false)))return nullptr;
        using Getter=sp<hidl::base::V1_0::IBase>(*)(const std::string&,const std::string&,bool,bool);
        static auto original=reinterpret_cast<Getter>(dlsym(RTLD_NEXT,"_ZN7android8hardware7details21getRawServiceInternalERKNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEESA_bb"));
        if(!original){fprintf(stderr,"API32_HIDL_STOCK_WAITER_UNAVAILABLE\n");return nullptr;}
        return original(descriptor,instance,retry,false);
    }
    if(instance=="default" &&
       (descriptor=="android.hardware.audio@7.0::IDevicesFactory" ||
        descriptor=="android.hardware.audio.effect@7.0::IEffectsFactory")) {
        static void* library=dlopen("/system/lib64/libpocket_audio_api32.so",RTLD_NOW|RTLD_LOCAL);
        using Factory=hidl::base::V1_0::IBase*(*)(const char*);
        static auto create=library?reinterpret_cast<Factory>(dlsym(library,"pocketCreateAudioService32")):nullptr;
        return create?create(descriptor.c_str()):nullptr;
    }
    // Host cameras: the in-process provider cameraserver lists through the HIDL manager.
    if(instance=="legacy/0" && descriptor=="android.hardware.camera.provider@2.4::ICameraProvider" &&
       property_get_bool("ro.pocket.camera.enabled",false)) {
        static void* library=dlopen("/system/lib64/libpocket_camera_api32.so",RTLD_NOW|RTLD_LOCAL);
        using Factory=hidl::base::V1_0::IBase*(*)(const char*);
        static auto create=library?reinterpret_cast<Factory>(dlsym(library,"HIDL_FETCH_ICameraProvider")):nullptr;
        if(!create)fprintf(stderr,"API32_CAMERA_PROVIDER_UNAVAILABLE error=%s\n",library?"symbol":dlerror());
        return create?create(instance.c_str()):nullptr;
    }
    if(instance=="default" &&
       (descriptor=="android.hardware.graphics.mapper@2.0::IMapper" ||
        descriptor=="android.hardware.graphics.allocator@2.0::IAllocator" ||
        descriptor=="android.hardware.graphics.composer@2.1::IComposer")) {
        static void* library=dlopen("/system/lib64/libpocket_graphics_api32.so",RTLD_NOW|RTLD_LOCAL);
        using Factory=hidl::base::V1_0::IBase*(*)(const char*);
        static auto create=library?reinterpret_cast<Factory>(dlsym(library,"pocketCreateGraphicsService32")):nullptr;
        if(create)return create(descriptor.c_str());
        fprintf(stderr,"API32_GRAPHICS_UNAVAILABLE descriptor=%s\n",descriptor.c_str());return nullptr;
    }
    if (instance == "default" &&
        (descriptor == "android.hidl.manager@1.0::IServiceManager" ||
         descriptor == "android.hidl.manager@1.1::IServiceManager" ||
         descriptor == "android.hidl.manager@1.2::IServiceManager" ||
         descriptor == "android.hardware.configstore@1.0::ISurfaceFlingerConfigs" ||
         descriptor == "android.hardware.configstore@1.1::ISurfaceFlingerConfigs" ||
         descriptor == "android.frameworks.sensorservice@1.0::ISensorManager" ||
         descriptor == "android.frameworks.displayservice@1.0::IDisplayService" ||
         descriptor == "android.frameworks.schedulerservice@1.0::ISchedulingPolicyService" ||
         descriptor == "android.frameworks.stats@1.0::IStats" ||
         descriptor == "android.hardware.keymaster@4.0::IKeymasterDevice" ||
         descriptor == "android.hardware.keymaster@4.1::IKeymasterDevice" ||
         descriptor == "android.hardware.gatekeeper@1.0::IGatekeeper" ||
         descriptor == "android.system.suspend@1.0::ISystemSuspend"))
        return pocketRemoteHardwareService(descriptor,instance);
    // Stock mediaswcodec's software Codec2 store, published in the Pocket registry.
    if (instance == "software" &&
        (descriptor == "android.hardware.media.c2@1.2::IComponentStore" ||
         descriptor == "android.hardware.media.c2@1.1::IComponentStore" ||
         descriptor == "android.hardware.media.c2@1.0::IComponentStore"))
        return pocketRemoteHardwareService(descriptor,instance);
    if (descriptor == "android.hardware.health@2.0::IHealth" &&
        (instance == "default" || instance == "backup"))
        return pocketRemoteHardwareService(descriptor,instance);
    fprintf(stderr,"API32_HAL_UNAVAILABLE %s/%s\n",descriptor.c_str(),instance.c_str());
    return nullptr;
}
}}}
