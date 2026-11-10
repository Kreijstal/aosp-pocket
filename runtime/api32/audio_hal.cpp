// SPDX-License-Identifier: GPL-3.0-only
// Matching stock HIDL wrappers over Pocket's legacy PCM HAL, in audioserver.
#include <android/hardware/audio/7.0/IDevicesFactory.h>
#include <android/hardware/audio/effect/7.0/IEffectsFactory.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

using android::sp;
using android::hidl::base::V1_0::IBase;
template<class Interface> static sp<IBase> load(const char* path,const char* symbol) {
    void* library=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    using Factory=Interface*(*)(const char*);
    auto create=library?reinterpret_cast<Factory>(dlsym(library,symbol)):nullptr;
    if(!create) {
        fprintf(stderr,"API32_AUDIO_HAL_LOAD_FAILED path=%s error=%s\n",path,dlerror());
        return nullptr;
    }
    return create("default");
}
extern "C" IBase* pocketCreateAudioService32(const char* descriptor) {
    if(!strcmp(descriptor,"android.hardware.audio@7.0::IDevicesFactory")) {
        static sp<IBase> factory=load<android::hardware::audio::V7_0::IDevicesFactory>(
            "/system/lib64/android.hardware.audio.legacy@7.0-impl.ranchu.so","HIDL_FETCH_IDevicesFactory");
        return factory.get();
    }
    if(!strcmp(descriptor,"android.hardware.audio.effect@7.0::IEffectsFactory")) {
        static sp<IBase> factory=load<android::hardware::audio::effect::V7_0::IEffectsFactory>(
            "/system/lib64/android.hardware.audio.effect@7.0-impl.so","HIDL_FETCH_IEffectsFactory");
        return factory.get();
    }
    return nullptr;
}
