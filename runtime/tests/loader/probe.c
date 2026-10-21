// SPDX-License-Identifier: GPL-3.0-only
#include <android/dlext.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc,char** argv) {
    if (argc!=3) return 64;
    int isolated=!strcmp(argv[1],"namespace"),denied=!strcmp(argv[1],"deny");
    android_dlextinfo ext={0};
    if (isolated || denied) {
        struct android_namespace_t*(*create)(const char*,const char*,const char*,uint64_t,const char*,struct android_namespace_t*)=
            dlsym(RTLD_DEFAULT,"android_create_namespace");
        int(*link_namespaces)(struct android_namespace_t*,struct android_namespace_t*,const char*)=
            dlsym(RTLD_DEFAULT,"android_link_namespaces");
        if (!create || !link_namespaces) return 65;
        struct android_namespace_t* ns=create("pocket-loader-regression",
            "/data/local/tmp/pocket-loader-tests/allowed:/mnt/expand/unused",
            NULL,1,"/data/local/tmp/pocket-loader-tests/allowed",NULL);
        if (!ns || !link_namespaces(ns,NULL,"libc.so:libdl.so")) {
            fprintf(stderr,"NAMESPACE_FAILED %s\n",dlerror());return 66;
        }
        ext.flags=0x200; // Android 9 ANDROID_DLEXT_USE_NAMESPACE (platform API).
        ext.library_namespace=ns;
    }
    dlerror();
    void* handle=!strcmp(argv[1],"plain") ? dlopen(argv[2],RTLD_NOW|RTLD_LOCAL)
        : android_dlopen_ext(argv[2],RTLD_NOW|RTLD_LOCAL,&ext);
    if (denied) {
        if (handle) { dlclose(handle);fputs("Namespace restriction lost\n",stderr);return 67; }
        const char* error=dlerror();
        if (!error || !strstr(error,"not accessible")) {
            fprintf(stderr,"Unexpected denial: %s\n",error?error:"no error");return 68;
        }
        puts("PASS: namespace rejects library outside permitted directory");return 0;
    }
    if (!handle) {
        const char* error=dlerror();fprintf(stderr,"LOAD_FAILED %s\n",error?error:"no loader error");return 1;
    }
    int(*answer)(void)=dlsym(handle,"pocket_loader_answer");
    int(*onload)(void*,void*)=dlsym(handle,"JNI_OnLoad");
    if (!answer || !onload || answer()!=42 || onload(NULL,NULL)!=0x00010006) return 2;
    puts("PASS: library loaded, marker=42, JNI_OnLoad resolved");
    dlclose(handle);return 0;
}
