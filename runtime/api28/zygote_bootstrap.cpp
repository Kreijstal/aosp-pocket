// SPDX-License-Identifier: GPL-3.0-only
#include <jni.h>
#include <jvmti.h>
#include <stdio.h>
#include "fd_utils.h"
namespace android { int pocketRegisterZygote(JNIEnv*); }
extern "C" JNIEXPORT void JNICALL
Java_dev_kreijstal_aosppocket_guest_ZygoteBootstrap_install(JNIEnv* env, jclass) {
    FileDescriptorWhitelist::Get()->Allow("/pocket-console");
    if (android::pocketRegisterZygote(env) != 0) env->FatalError("Rootless zygote registration failed");
    fputs("POCKET_ZYGOTE_NATIVE_REGISTERED source=android-9.0.0_r61\n", stderr);
}

// Android 9's image loader uses the boot-image dex list even when BOOTCLASSPATH
// includes another JAR. Use ART's supported JVMTI append operation at VM startup.
extern "C" JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* vm, char*, void*) {
    jvmtiEnv* tools = nullptr;
    jint result = vm->GetEnv(reinterpret_cast<void**>(&tools), JVMTI_VERSION_1_2);
    if (result != JNI_OK || !tools) {
        fprintf(stderr, "POCKET_BOOTCLASSPATH_FAILED GetEnv=%d\n", result);
        return JNI_ERR;
    }
    jvmtiError error = tools->AddToBootstrapClassLoaderSearch("/system/framework/pocket-zygote.jar");
    fprintf(stderr, "POCKET_BOOTCLASSPATH_APPEND jvmti_error=%d\n", error);
    return error == JVMTI_ERROR_NONE ? JNI_OK : JNI_ERR;
}
