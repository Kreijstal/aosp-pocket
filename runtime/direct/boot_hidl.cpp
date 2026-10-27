// SPDX-License-Identifier: GPL-3.0-only
// Explicit in-process software-graphics HAL profile. Never uses host hwbinder.
#ifndef POCKET_GUEST_API
#define POCKET_GUEST_API 28
#endif
#include <android/hidl/base/1.0/IBase.h>
#include <dlfcn.h>
#include "SurfaceFlingerConfigs.h"
#include "boot_hardware.h"
#include "boot_media.h"
#include "boot_vibrator.h"
#include <stdio.h>
#include <string>
#include <utils/StrongPointer.h>
#if POCKET_GUEST_API == 29
#include <vndksupport/linker.h>
#endif
namespace android { namespace hardware { namespace details {
sp<::android::hidl::base::V1_0::IBase> getRawServiceInternal(
        const std::string& descriptor,const std::string& instance,bool retry,bool getStub) {
    (void)retry; (void)getStub;
    using ::android::hidl::base::V1_0::IBase;
#ifdef POCKET_MEDIA_CODECS
    if ((descriptor=="android.hardware.media.omx@1.0::IOmx" ||
         descriptor=="android.hardware.media.omx@1.0::IOmxStore") && instance=="default")
        return pocketRemoteHardwareService(descriptor,instance);
    if (auto memory=pocketMediaMemoryService(descriptor,instance)) return memory;
#endif
    if(POCKET_GUEST_API==29 && descriptor=="android.system.suspend@1.0::ISystemSuspend" && instance=="default")
        return pocketRemoteHardwareService(descriptor,instance);
    // VibratorService fetches each interface version separately; one 1.2 object serves all three.
    if ((descriptor=="android.hardware.vibrator@1.0::IVibrator" || descriptor=="android.hardware.vibrator@1.1::IVibrator" ||
         descriptor=="android.hardware.vibrator@1.2::IVibrator") && instance=="default") return pocketHostVibrator();
    if ((descriptor=="android.hardware.configstore@1.0::ISurfaceFlingerConfigs" ||
         descriptor=="android.hardware.configstore@1.1::ISurfaceFlingerConfigs") && instance=="default") {
        // Java HwBinder requires a real Bn/Bp transport even for this software
        // HAL; returning the local implementation exposes an invalid parcel.
        if (getenv("POCKET_SYSCALL_BACKEND")) return pocketRemoteHardwareService(descriptor,instance);
        fprintf(stderr,"DIRECT_HAL_CONFIGSTORE %s/default source=android-9.0.0_r61\n",descriptor.c_str());
        return new ::android::hardware::configstore::V1_1::implementation::SurfaceFlingerConfigs();
    }
    if ((descriptor=="android.hardware.health@2.0::IHealth" && (instance=="default" || instance=="backup")) ||
        ((descriptor=="android.hidl.manager@1.0::IServiceManager" || descriptor=="android.hidl.manager@1.1::IServiceManager" ||
          (POCKET_GUEST_API==29 && descriptor=="android.hidl.manager@1.2::IServiceManager")) && instance=="default")) return pocketRemoteHardwareService(descriptor,instance);
    if(descriptor=="android.hardware.camera.provider@2.4::ICameraProvider" && instance=="legacy/0") {
        // Only cameraserver asks; the provider streams host cameras through the broker.
        void* handle=dlopen("libpocket_camera_api28.so",RTLD_NOW|RTLD_LOCAL);
        if(!handle) {fprintf(stderr,"POCKET_CAMERA_PROVIDER_LOAD_FAILED %s\n",dlerror());return nullptr;}
        using Fetch=IBase*(*)(const char*);
        auto fetch=reinterpret_cast<Fetch>(dlsym(handle,"HIDL_FETCH_ICameraProvider"));
        return fetch?sp<IBase>(fetch(instance.c_str())):nullptr;
    }
    if(descriptor=="android.hardware.keymaster@3.0::IKeymasterDevice" && (instance=="default" || instance=="softwareonly")) {
#if POCKET_GUEST_API == 29
        void* handle=android_load_sphal_library("/vendor/lib64/hw/android.hardware.keymaster@3.0-impl.so",RTLD_NOW|RTLD_LOCAL);
#else
        void* handle=dlopen("/vendor/lib64/hw/android.hardware.keymaster@3.0-impl.so",RTLD_NOW|RTLD_LOCAL);
#endif
        if(!handle) {fprintf(stderr,"DIRECT_KEYMASTER_LOAD_FAILED %s\n",dlerror());return nullptr;}
        using Fetch=IBase*(*)(const char*);
        auto fetch=reinterpret_cast<Fetch>(dlsym(handle,"HIDL_FETCH_IKeymasterDevice"));
        sp<IBase> service=fetch?fetch("softwareonly"):nullptr;
        fprintf(stderr,"DIRECT_HAL_SOFTWARE_KEYMASTER available=%d hardware_device=unused\n",service!=nullptr);
        return service;
    }
#ifdef POCKET_GUEST_AUDIO
    // In-process passthrough audio HALs inside audioserver; the primary module is audio.primary.pocket.
    bool audio=(descriptor=="android.hardware.audio@2.0::IDevicesFactory" ||
                descriptor=="android.hardware.audio.effect@2.0::IEffectsFactory") && instance=="default";
#else
    bool audio=false;
#endif
    if(POCKET_GUEST_API==29 && instance=="default" &&
        (descriptor=="android.hardware.audio@4.0::IDevicesFactory" ||
         descriptor=="android.hardware.audio.effect@4.0::IEffectsFactory"))audio=true;
    bool softwareGatekeeper=POCKET_GUEST_API==29 && instance=="default" &&
        descriptor=="android.hardware.gatekeeper@1.0::IGatekeeper";
    // Retain a precise profile; unsupported HALs remain unavailable.
    bool mapper21=POCKET_GUEST_API==29 && descriptor=="android.hardware.graphics.mapper@2.1::IMapper";
    if (!audio && !softwareGatekeeper && !mapper21 && descriptor!="android.hardware.graphics.composer@2.1::IComposer" &&
        descriptor!="android.hardware.graphics.allocator@2.0::IAllocator" &&
        descriptor!="android.hardware.graphics.mapper@2.0::IMapper") {
        fprintf(stderr,"DIRECT_HAL_UNAVAILABLE %s/%s\n",descriptor.c_str(),instance.c_str());
        return nullptr;
    }
    size_t separator=descriptor.find("::");
    std::string library="/vendor/lib64/hw/"+descriptor.substr(0,separator)+"-impl.so";
    if(POCKET_GUEST_API==29 && (mapper21 || descriptor=="android.hardware.graphics.mapper@2.0::IMapper"))
        library="/vendor/lib64/hw/android.hardware.graphics.mapper@2.0-impl-2.1.so";
    // Android 10 keeps the software vendor HALs in the SP-HAL namespace.
    // Match AOSP's passthrough service loader so its VNDK dependencies retain
    // their own namespace rather than mixing them with framework libraries.
#if POCKET_GUEST_API == 29
    void* handle=android_load_sphal_library(library.c_str(),RTLD_NOW|RTLD_LOCAL);
#else
    void* handle=dlopen(library.c_str(),RTLD_NOW|RTLD_LOCAL);
#endif
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
