// SPDX-License-Identifier: GPL-3.0-only
// Android 12L system_server hook for microG package-query compatibility.
// tools/build_api32_microg.py appends this file to the patched
// com_android_internal_os_Zygote.cpp and calls it in the system_server child
// right after nativeForkSystemServer's specialization, as the API 28 zygote
// does with MicrogSupport.install().
//
// MicrogSupport32 lives in its own jar, outside BOOTCLASSPATH, so the boot
// class path checksums recorded in the image's compiled code stay unchanged.
// An image without the jar is unaffected, and a failing hook is reported
// without stopping system_server (Android 12L then runs without microG).
#include <jni.h>
#include <stdio.h>
#include <unistd.h>

extern "C" void pocketSystemServerMicrog32(JNIEnv* env) {
    static const char kJar[]="/system/framework/pocket-microg.jar";
    if(access(kJar,R_OK)!=0)return;
    jclass base=env->FindClass("java/lang/ClassLoader");
    jclass path=env->FindClass("dalvik/system/PathClassLoader");
    jmethodID system=base?env->GetStaticMethodID(base,"getSystemClassLoader","()Ljava/lang/ClassLoader;"):nullptr;
    jmethodID create=path?env->GetMethodID(path,"<init>","(Ljava/lang/String;Ljava/lang/ClassLoader;)V"):nullptr;
    jmethodID load=base?env->GetMethodID(base,"loadClass","(Ljava/lang/String;)Ljava/lang/Class;"):nullptr;
    jobject parent=system?env->CallStaticObjectMethod(base,system):nullptr;
    jstring jar=env->NewStringUTF(kJar);
    jobject loader=(create && parent && !env->ExceptionCheck())?env->NewObject(path,create,jar,parent):nullptr;
    jstring name=env->NewStringUTF("dev.kreijstal.aosppocket.guest.MicrogSupport32");
    jclass type=(loader && load && !env->ExceptionCheck())?static_cast<jclass>(env->CallObjectMethod(loader,load,name)):nullptr;
    jmethodID install=(type && !env->ExceptionCheck())?env->GetStaticMethodID(type,"install","()V"):nullptr;
    if(install)env->CallStaticVoidMethod(type,install);
    if(!install || env->ExceptionCheck()) {
        if(env->ExceptionCheck()){env->ExceptionDescribe();env->ExceptionClear();}
        fputs("POCKET_MICROG_UNAVAILABLE system_server continues without microG package compatibility\n",stderr);
    }
}
