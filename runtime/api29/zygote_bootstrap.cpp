// SPDX-License-Identifier: GPL-3.0-only
#include <jni.h>
#include <stdio.h>
#include "fd_utils.h"
namespace android { int pocketRegisterZygote29(JNIEnv*); }

extern "C" JNIEXPORT void JNICALL
Java_dev_kreijstal_aosppocket_guest_ZygoteBootstrap29_install(JNIEnv* env,jclass) {
    FileDescriptorWhitelist::Get()->Allow("/tmp/properties.log");
    FileDescriptorWhitelist::Get()->Allow("/logs/manager-init.log");
    if (android::pocketRegisterZygote29(env)!=JNI_OK)
        env->FatalError("Android 10 rootless zygote registration failed");
    fputs("API29_ZYGOTE_NATIVE_REGISTERED source=android-10.0.0_r47\n",stderr);
}
