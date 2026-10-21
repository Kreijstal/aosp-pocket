// SPDX-License-Identifier: GPL-3.0-only
#include <jni.h>
#include <jvmti.h>
#include <stdio.h>
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <stdint.h>
#include <string.h>
#ifdef POCKET_ZYGISK
#include "pocket_art_bridge.h"
#endif
#include "fd_utils.h"
namespace android { int pocketRegisterZygote(JNIEnv*); }
extern "C" int pocketInstallZygisk(JNIEnv* env, const JNINativeMethod* methods, size_t count) {
#ifdef POCKET_ZYGISK
    char enabled[PROP_VALUE_MAX] = {};
    __system_property_get("pocket.zygisk.enabled", enabled);
    if (enabled[0] != '1') return 0;
    void* handle = dlopen("/system/lib64/libpocket_zygisk.so", RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr,"POCKET_ZYGISK_LOAD_FAILED %s\n",dlerror());
        return -1;
    }
    // Reuse ART's real native-bridge introspection functions for the public
    // hookJniNativeMethods API. Verify every pinned entry before calling it.
    void* art=dlopen("/system/lib64/libart.so",RTLD_NOW | RTLD_LOCAL);
    void* anchor=art?dlsym(art,"_ZN3art16LoadNativeBridgeERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE"):nullptr;
    Dl_info image{};
    if(!anchor || !dladdr(anchor,&image)) return -1;
    void* callbacks[3];
    for(size_t i=0;i<3;++i) {
        auto address=static_cast<unsigned char*>(image.dli_fbase)+pocket_art_callbacks[i].offset;
        if(memcmp(address,pocket_art_callbacks[i].bytes,32)) {
            fputs("POCKET_ZYGISK_ART_CALLBACK_MISMATCH\n",stderr);
            return -1;
        }
        callbacks[i]=address;
    }
    using Install = int(*)(JNIEnv*, const JNINativeMethod*, size_t, const void*);
    auto install = reinterpret_cast<Install>(dlsym(handle,"pocket_zygisk_install"));
    int result = install ? install(env,methods,count,callbacks) : -1;
    fprintf(stderr,"POCKET_ZYGISK_INSTALL result=%d\n",result);
    // Keep the handle for the zygote lifetime and forked children.
    return result;
#else
    (void)env; (void)methods; (void)count;
    return 0;
#endif
}
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
