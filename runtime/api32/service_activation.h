// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace pocket {
struct LazyService32 {unsigned slot;const char* init;bool requiresMedia;};
inline const LazyService32* lazyService32(const char* name,bool hardware=false,bool metricsInCore=false) {
    static constexpr LazyService32 metrics{0,"pocket-mediametrics",false},
        coreMetrics{0,"pocket-registry",false},
        player{1,"pocket-mediaserver",true},extractor{2,"pocket-mediaextractor",true},
        camera{3,"pocket-cameraserver",false},codec{4,"pocket-mediaswcodec",true};
    if(!name)return nullptr;
    if(hardware) {
        if(!strcmp(name,"android.hardware.media.c2@1.0::IComponentStore/software") ||
           !strcmp(name,"android.hardware.media.c2@1.1::IComponentStore/software") ||
           !strcmp(name,"android.hardware.media.c2@1.2::IComponentStore/software"))return &codec;
        if(!strcmp(name,"android.frameworks.cameraservice.service@2.0::ICameraService/default") ||
           !strcmp(name,"android.frameworks.cameraservice.service@2.1::ICameraService/default") ||
           !strcmp(name,"android.frameworks.cameraservice.service@2.2::ICameraService/default"))return &camera;
        return nullptr;
    }
    if(!strcmp(name,"media.metrics"))return metricsInCore?&coreMetrics:&metrics;
    if(!strcmp(name,"media.player") || !strcmp(name,"media.resource_manager"))return &player;
    if(!strcmp(name,"media.extractor"))return &extractor;
    if(!strcmp(name,"media.camera"))return &camera;
    return nullptr;
}
// Bound concurrent/missing-service retries per daemon, including aliases.
// Invoke init outside this lock: registration may call back into the registry.
class LazyServiceStarter32 {
    std::mutex mutex_;
    std::array<uint64_t,5> last_{};
    std::array<bool,5> used_{},active_{};
public:
    template<class Start> bool request(const LazyService32* service,bool enabled,bool media,
                                      uint64_t now,Start start) {
        if(!service || !enabled || (service->requiresMedia && !media))return false;
        unsigned slot=service->slot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(active_[slot] || (used_[slot] && now>=last_[slot] && now-last_[slot]<1000))return false;
            active_[slot]=used_[slot]=true;last_[slot]=now;
        }
        start(service->init);
        {std::lock_guard<std::mutex> lock(mutex_);active_[slot]=false;}
        return true;
    }
};
}
