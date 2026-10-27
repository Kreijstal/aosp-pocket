// SPDX-License-Identifier: GPL-3.0-only
// ART's DexFile API defines a small, closed platform extension in the boot
// class table. No agent, instrumentation plugin, runtime patch or ART offset.
#include "boot_classes.h"
#include <stdio.h>
#include <string.h>

static jobject bootDex;
static void reject(JNIEnv* env,const char* message) {
    if(env->ExceptionCheck())env->ExceptionDescribe();
    fprintf(stderr,"POCKET_BOOT_CLASSES_FAILED %s\n",message);
    env->FatalError(message);
}

void pocketLoadBootClasses(JNIEnv* env) {
    FILE* names=fopen("/system/framework/pocket-zygote.classes","re");
    if(!names) {reject(env,"Missing bootstrap definition manifest");return;}
    char line[512];
    if(!fgets(line,sizeof(line),names) || strcmp(line,"PCKBOOT1\n")) {
        fclose(names);reject(env,"Invalid bootstrap definition manifest");return;
    }
    jclass dexClass=env->FindClass("dalvik/system/DexFile");
    jmethodID ctor=dexClass?env->GetMethodID(dexClass,"<init>","(Ljava/lang/String;)V"):nullptr;
    jmethodID load=dexClass?env->GetMethodID(dexClass,"loadClass","(Ljava/lang/String;Ljava/lang/ClassLoader;)Ljava/lang/Class;"):nullptr;
    if(!ctor || !load || env->ExceptionCheck()) {fclose(names);reject(env,"ART DexFile API unavailable");return;}
    jstring path=env->NewStringUTF("/system/framework/pocket-zygote.jar");
    jobject dex=env->NewObject(dexClass,ctor,path);
    env->DeleteLocalRef(path);
    if(!dex || env->ExceptionCheck()) {fclose(names);reject(env,"Cannot open bootstrap DEX");return;}
    unsigned count=0;
    while(fgets(line,sizeof(line),names)) {
        size_t n=strlen(line);
        if(n<2 || line[n-1]!='\n' || ++count>1024) {fclose(names);reject(env,"Invalid bootstrap definition entry");return;}
        line[--n]=0;
        static const char prefix[]="dev.kreijstal.aosppocket.";
        if(strncmp(line,prefix,sizeof(prefix)-1)) {fclose(names);reject(env,"Non-Pocket bootstrap definition");return;}
        for(size_t i=0;i<n;i++)if(!((line[i]>='a' && line[i]<='z') || (line[i]>='A' && line[i]<='Z') ||
              (line[i]>='0' && line[i]<='9') || line[i]=='.' || line[i]=='_' || line[i]=='$')) {
            fclose(names);reject(env,"Invalid bootstrap class name");return;
        }
        jstring name=env->NewStringUTF(line);
        jobject klass=env->CallObjectMethod(dex,load,name,nullptr);
        env->DeleteLocalRef(name);
        if(!klass || env->ExceptionCheck()) {fclose(names);reject(env,line);return;}
        env->DeleteLocalRef(klass);
    }
    bool error=ferror(names);fclose(names);
    if(error || !count) {reject(env,"Incomplete bootstrap definition manifest");return;}
    // ART also roots registered DexFiles in the boot class table. Keep an
    // explicit JNI reference so ownership is clear for the whole VM lifetime.
    bootDex=env->NewGlobalRef(dex);env->DeleteLocalRef(dex);env->DeleteLocalRef(dexClass);
    if(!bootDex || env->ExceptionCheck()) {reject(env,"Cannot retain bootstrap DEX");return;}
    fprintf(stderr,"POCKET_BOOT_CLASSES_READY classes=%u loader=boot jvmti=unused\n",count);
}
