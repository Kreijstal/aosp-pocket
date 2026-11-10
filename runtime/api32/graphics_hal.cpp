// SPDX-License-Identifier: GPL-3.0-only
// The pinned 12L passthrough implementations over this profile's software HALs.
#define LOG_TAG "PocketGraphics32"
#include <mapper-passthrough/2.0/GrallocLoader.h>
#include <allocator-passthrough/2.0/GrallocLoader.h>
#include <composer-passthrough/2.1/HwcLoader.h>
#include <string.h>

using android::sp;
using android::hidl::base::V1_0::IBase;
namespace graphics=android::hardware::graphics;
extern "C" IBase* pocketCreateGraphicsService32(const char* descriptor) {
    if(!strcmp(descriptor,"android.hardware.graphics.mapper@2.0::IMapper")) {
        static sp<IBase> mapper=graphics::mapper::V2_0::passthrough::GrallocLoader::load();return mapper.get();
    }
    if(!strcmp(descriptor,"android.hardware.graphics.allocator@2.0::IAllocator")) {
        static sp<IBase> allocator=graphics::allocator::V2_0::passthrough::GrallocLoader::load();return allocator.get();
    }
    if(!strcmp(descriptor,"android.hardware.graphics.composer@2.1::IComposer")) {
        static sp<IBase> composer=graphics::composer::V2_1::passthrough::HwcLoader::load();return composer.get();
    }
    return nullptr;
}
