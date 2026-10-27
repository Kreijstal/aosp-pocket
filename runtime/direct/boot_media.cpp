// SPDX-License-Identifier: GPL-3.0-only
// Real Android 9 OMX implementations on Pocket's private Binder transport.
#include "boot_media.h"
#include <android/hardware/media/omx/1.0/BnHwOmx.h>
#include <android/hardware/media/omx/1.0/BnHwOmxStore.h>
#include <media/stagefright/omx/1.0/Omx.h>
#include <media/stagefright/omx/1.0/OmxStore.h>
#include <media/stagefright/MediaCodecList.h>
#include "AshmemMapper.h"
#include "AshmemAllocator.h"

android::sp<android::hardware::IBinder> pocketSoftwareMediaService(const std::string& descriptor) {
    using namespace android;
    using namespace android::hardware::media::omx::V1_0;
    if (descriptor == IOmx::descriptor) {
        static sp<hardware::IBinder> omx = new BnHwOmx(new implementation::Omx);
        return omx;
    }
    if (descriptor == IOmxStore::descriptor) {
        static sp<hardware::IBinder> store = new BnHwOmxStore(new implementation::OmxStore);
        return store;
    }
    return nullptr;
}

android::sp<android::hidl::base::V1_0::IBase> pocketMediaMemoryService(
        const std::string& descriptor, const std::string& instance) {
    using namespace android;
    if (instance != "ashmem") return nullptr;
    if (descriptor == "android.hidl.memory@1.0::IMapper") {
        static sp<hidl::base::V1_0::IBase> mapper = new hidl::memory::V1_0::implementation::AshmemMapper;
        return mapper;
    }
    if (descriptor == "android.hidl.allocator@1.0::IAllocator") {
        static sp<hidl::base::V1_0::IBase> allocator = new hidl::allocator::V1_0::implementation::AshmemAllocator;
        return allocator;
    }
    return nullptr;
}

namespace android {
// This profile has an independent software OMX backend, without MediaPlayer
// or AudioFlinger. Build the actual platform catalog from private IOmxStore
// and live capability queries instead of waiting for absent media.player.
sp<IMediaCodecList> MediaCodecList::getInstance() {
    return getLocalInstance();
}
}
