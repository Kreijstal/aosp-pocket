// SPDX-License-Identifier: GPL-3.0-only
// Optional guest-only startup: no ptrace, host root or Magisk injection.
#include <binder/IServiceManager.h>
#include <pthread.h>
#include <unistd.h>
#define LOG_SUB "pocket/api28"
#include "logging.hpp"

extern "C" bool entry(void*);
extern "C" bool teesim_control_load_file(const char*);

static void* wait_for_keystore(void*) {
    // The constructor runs before keystore registers its Binder service.
    // Wait without installing a hook repeatedly or delaying its main thread.
    for (int i = 0; i < 300; ++i) {
        auto sm = android::defaultServiceManager();
        if (sm && sm->checkService(android::String16("android.security.keystore"))) {
            if (entry(nullptr)) {
                const bool configured = teesim_control_load_file(
                    "/data/misc/keystore/pocket-teesim.json");
                LOGI("optional Android 9 backend installed=1 configured=%d", configured);
            } else {
                LOGE("optional Android 9 backend could not install");
            }
            return nullptr;
        }
        usleep(100000);
    }
    LOGE("keystore service unavailable; backend left inactive");
    return nullptr;
}

__attribute__((constructor)) static void pocket_teesim_start() {
    pthread_t thread;
    if (pthread_create(&thread, nullptr, wait_for_keystore, nullptr) == 0)
        pthread_detach(thread);
    else
        LOGE("cannot start optional backend; normal keystore remains active");
}
