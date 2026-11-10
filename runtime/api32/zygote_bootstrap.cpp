// SPDX-License-Identifier: GPL-3.0-only
#include <jni.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <cutils/trace.h>
#include <android_runtime/AndroidRuntime.h>
#include "fd_utils.h"
#include "guest_api.h"
#include "../api28/binder_identity.h"
#include "../direct/kernel_identity.h"
namespace android { int pocketRegisterZygote32(JNIEnv*); }
extern "C" void pocketSystemServerMicrog32(JNIEnv*);
namespace {
bool systemServerChild=false;
void guestZygoteInit(JNIEnv* env,jclass) {
    // Preserve the stock nativeZygoteInit implementation's virtual call.
    auto* runtime=android::AndroidRuntime::getRuntime();
    if(!runtime){env->FatalError("Missing AndroidRuntime in zygote child");return;}
    runtime->onZygoteInit();
    if(systemServerChild)pocketSystemServerMicrog32(env);
    const char* enabled=getenv("POCKET_API32_SYSTEM_SERVER_BRIDGE");
    if(!systemServerChild || !enabled || strcmp(enabled,"1"))return;
    jclass bootstrap=env->FindClass("dev/kreijstal/aosppocket/guest/ZygoteBootstrap32");
    jmethodID start=bootstrap?env->GetStaticMethodID(bootstrap,"startSystemServerBridge","()V"):nullptr;
    if(start)env->CallStaticVoidMethod(bootstrap,start);
    if(!start || env->ExceptionCheck()) {
        env->ExceptionDescribe();env->FatalError("Cannot start the system-server bridge");
    }
    if(bootstrap)env->DeleteLocalRef(bootstrap);
}
}
namespace android {
// No JNI, allocation or thread creation while ART is still in its fork region.
void pocketSystemServerChild32(){systemServerChild=true;}
}

// Keep the platform parser, exceptions and all non-identity measurements.
// Process.getUidForPid is Java code that calls this JNI method. Framework
// liveness checks must compare guest roles, while host Binder authentication
// continues reading the unmodified kernel /proc files directly.
void android_os_Process_readProcLines(JNIEnv*,jobject,jstring,jobjectArray,jlongArray);
static void guestReadProcLines(JNIEnv* env,jobject type,jstring path,jobjectArray fields,jlongArray values) {
    android_os_Process_readProcLines(env,type,path,fields,values);
    if(env->ExceptionCheck())return;
    const char* name=env->GetStringUTFChars(path,nullptr);if(!name)return;
    unsigned pid=0;int end=0;
    bool status=sscanf(name,"/proc/%u/status%n",&pid,&end)==1 && end>0 && !name[end] && pid>0 && pid<=INT_MAX;
    env->ReleaseStringUTFChars(path,name);
    if(!status)return;
    uint64_t identity=0;
    if(!pocket_binder_identity(static_cast<pid_t>(pid),&identity) || (identity>>32)>INT_MAX)return;
    const uid_t kernelUid=pocket_kernel_uid_from_proc();
    if(kernelUid==static_cast<uid_t>(-1))return;
    for(jsize i=0;i<env->GetArrayLength(fields);i++) {
        jstring field=static_cast<jstring>(env->GetObjectArrayElement(fields,i));
        if(!field)return;
        const char* label=env->GetStringUTFChars(field,nullptr);
        if(!label){env->DeleteLocalRef(field);return;}
        bool uid=!strcmp(label,"Uid:");
        env->ReleaseStringUTFChars(field,label);env->DeleteLocalRef(field);
        if(uid) {
            jlong actual;env->GetLongArrayRegion(values,i,1,&actual);
            if(env->ExceptionCheck())return;
            // A missing/unreadable process or a different host UID must retain
            // the platform result. PID start-time validation rejects stale records.
            if(actual==kernelUid) {
                jlong logical=static_cast<jlong>(identity>>32);
                env->SetLongArrayRegion(values,i,1,&logical);
            }
        }
    }
}

extern "C" JNIEXPORT void JNICALL
Java_dev_kreijstal_aosppocket_guest_ZygoteBootstrap32_install(JNIEnv* env,jclass) {
    if (getuid()!=0 || pocket_native_setgroups(0,nullptr))
        env->FatalError("Android 12L zygote requires the active rootless loader");
    // This guest cannot administer host kernel tracing. Some Android hosts
    // permit opening trace_marker but deny fstat, so retaining libcutils' early
    // tracing fd also prevents zygote's mandatory descriptor validation.
    // Disable tags first and retire the descriptor owned by libcutils. Its
    // once-only initialization has already run during AndroidRuntime startup;
    // the negative marker also prevents host writes if a child enables tags.
    atrace_set_tracing_enabled(false);
    if (atrace_marker_fd >= 0) {
        close(atrace_marker_fd);
        atrace_marker_fd = -1;
    }
    fputs("API32_KERNEL_TRACING_DISABLED rootless=1\n",stderr);
    for (const char* path:{"/tmp/properties.log","/tmp/zygote.log","/logs/manager-init.log"})
        FileDescriptorAllowlist::Get()->Allow(path);
    if (android::pocketRegisterZygote32(env)!=JNI_OK)
        env->FatalError("Android 12L zygote JNI registration failed");
    jclass process=env->FindClass("android/os/Process");
    JNINativeMethod method={const_cast<char*>("readProcLines"),const_cast<char*>("(Ljava/lang/String;[Ljava/lang/String;[J)V"),
        reinterpret_cast<void*>(guestReadProcLines)};
    if(!process || env->RegisterNatives(process,&method,1)!=JNI_OK)
        env->FatalError("Android 12L guest process identity registration failed");
    env->DeleteLocalRef(process);
    jclass zygoteInit=env->FindClass("com/android/internal/os/ZygoteInit");
    JNINativeMethod childInit={const_cast<char*>("nativeZygoteInit"),const_cast<char*>("()V"),
        reinterpret_cast<void*>(guestZygoteInit)};
    if(!zygoteInit || env->RegisterNatives(zygoteInit,&childInit,1)!=JNI_OK)
        env->FatalError("Android 12L child initialization registration failed");
    env->DeleteLocalRef(zygoteInit);
    fputs("API32_ZYGOTE_NATIVE_REGISTERED source=android-platform-12.1.0_r23\n",stderr);
}

extern "C" JNIEXPORT jint JNICALL
Java_dev_kreijstal_aosppocket_guest_Api32Child_kernelUid(JNIEnv*,jclass) {
    return static_cast<jint>(pocket_kernel_uid_from_proc());
}

extern "C" JNIEXPORT void JNICALL
Java_dev_kreijstal_aosppocket_guest_Api32Child_rawProcLines(JNIEnv* env,jclass type,jstring path,jobjectArray fields,jlongArray values) {
    android_os_Process_readProcLines(env,type,path,fields,values);
}

extern "C" JNIEXPORT jintArray JNICALL
Java_dev_kreijstal_aosppocket_guest_Api32Child_groups(JNIEnv* env,jclass) {
    gid_t groups[64];
    int count=getgroups(64,groups);
    if (count<0) {env->FatalError("Cannot read logical guest groups");return nullptr;}
    jint values[64];
    for(int i=0;i<count;i++)values[i]=static_cast<jint>(groups[i]);
    jintArray result=env->NewIntArray(count);
    if(result)env->SetIntArrayRegion(result,0,count,values);
    return result;
}
