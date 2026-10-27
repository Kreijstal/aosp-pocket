// SPDX-License-Identifier: GPL-3.0-only
#include <jni.h>
#include <stdio.h>
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <cutils/sched_policy.h>
#ifdef POCKET_ZYGISK
#include "pocket_art_bridge.h"
#endif
#include "fd_utils.h"
namespace android { int pocketRegisterZygote(JNIEnv*); }

extern "C" void pocketSystemServerCompatibility(JNIEnv* env) {
    jclass type=env->FindClass("dev/kreijstal/aosppocket/guest/MicrogSupport");
    jmethodID install=type?env->GetStaticMethodID(type,"install","()V"):nullptr;
    if(install)env->CallStaticVoidMethod(type,install);
    if(!install || env->ExceptionCheck())env->FatalError("Guest package compatibility bootstrap failed");
    env->DeleteLocalRef(type);
}

// A guest cannot move host threads between privileged cgroups. Treat the
// framework's cpuset request as a scheduling hint, retaining host assignment.
// Keep invalid policy/thread errors instead of hiding unrelated API misuse.
static void guestThreadGroupAndCpuset(JNIEnv* env, jclass, jint tid, jint group) {
    if (!tid) tid = static_cast<jint>(syscall(SYS_gettid));
    char path[64];snprintf(path,sizeof(path),"/proc/%d/status",tid);
    struct stat status{};
    if (tid < 0 || group < SP_DEFAULT || group > SP_MAX || stat(path,&status)) {
        jclass error=env->FindClass("java/lang/IllegalArgumentException");
        if (error) env->ThrowNew(error,"Invalid guest thread or scheduling group");
        return;
    }
    static bool reported=false;
    if (!__atomic_exchange_n(&reported,true,__ATOMIC_RELAXED))
        fputs("DIRECT_THREAD_GROUP_HINT host_cpuset=unchanged\n",stderr);
}
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
    // A syscall backend exposes the session log's actual guest pathname.
    // Accept only a named log directly below /logs, configured before startup.
    const char* console=getenv("POCKET_GUEST_CONSOLE");
    if(console && !strncmp(console,"/logs/",6) && console[6] &&
       !strchr(console+6,'/') && !strstr(console+6,".."))
        FileDescriptorWhitelist::Get()->Allow(console);
    if (android::pocketRegisterZygote(env) != 0) env->FatalError("Rootless zygote registration failed");
    const char* backend=getenv("POCKET_SYSCALL_BACKEND");
    if (backend && !strcmp(backend,"native")) {
        jclass process=env->FindClass("android/os/Process");
        JNINativeMethod method={const_cast<char*>("setThreadGroupAndCpuset"),
            const_cast<char*>("(II)V"),reinterpret_cast<void*>(guestThreadGroupAndCpuset)};
        if (!process || env->RegisterNatives(process,&method,1))
            env->FatalError("Guest scheduling adapter registration failed");
        env->DeleteLocalRef(process);
    }
    fputs("POCKET_ZYGOTE_NATIVE_REGISTERED source=android-9.0.0_r61\n", stderr);
}
