// SPDX-License-Identifier: GPL-3.0-only
// Preloaded into the 12L guest audioserver.
//
// AudioFlinger and AudioPolicyService arm android::TimeCheck around every
// incoming Binder call, and the watchdog aborts the whole audioserver if the
// call takes more than 5 s. Some of those calls make outgoing Binder calls into
// the guest system_server while holding AudioFlinger's lock. For example,
// createTrack -> OpPlayAudioMonitor -> AppOpsManager.startWatchingMode.
// While the guest is starting apps on a slow or memory-pressured host, those
// calls have stalled for more than 5 s. The abort then kills every audio
// client, and the AudioTrack that triggered it fails with "Cannot create
// AudioTrack".
//
// Keep the watchdog for real deadlocks, but give it a floor that tolerates load
// stalls. Both libraries import the constructor through a PLT slot, so this
// definition takes precedence and forwards to libmediautils.
#include <android/log.h>
#include <dlfcn.h>
#include <stdint.h>

#define POCKET_TIMECHECK_FLOOR_MS 20000u
#define CTOR "_ZN7android9TimeCheckC1EPKcj"

typedef void (*timecheck_ctor)(void* self, const char* tag, uint32_t timeout_ms);

static timecheck_ctor real_ctor(void) {
    static timecheck_ctor cached;
    timecheck_ctor ctor = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);
    if (!ctor) {
        ctor = (timecheck_ctor)dlsym(RTLD_NEXT, CTOR);
        if (!ctor) __android_log_assert(NULL, "PocketTimeCheck", "missing %s: %s", CTOR, dlerror());
        __atomic_store_n(&cached, ctor, __ATOMIC_RELEASE);
        __android_log_print(ANDROID_LOG_INFO, "PocketTimeCheck",
            "TimeCheck floor active: timeouts below %u ms raised to %u ms",
            POCKET_TIMECHECK_FLOOR_MS, POCKET_TIMECHECK_FLOOR_MS);
    }
    return ctor;
}

__attribute__((visibility("default")))
void _ZN7android9TimeCheckC1EPKcj(void* self, const char* tag, uint32_t timeout_ms) {
    if (timeout_ms < POCKET_TIMECHECK_FLOOR_MS) timeout_ms = POCKET_TIMECHECK_FLOOR_MS;
    real_ctor()(self, tag, timeout_ms);
}
