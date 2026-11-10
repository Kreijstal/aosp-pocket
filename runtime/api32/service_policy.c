// SPDX-License-Identifier: GPL-3.0-only
// A rootless profile has no independently loaded guest SELinux policy. These
// process-local APIs describe that fact; the host app domain remains enforced.
#include <selinux/selinux.h>
#include <selinux/android.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__attribute__((constructor)) static void require_guest32(void) {
    char sdk[PROP_VALUE_MAX];
    if(syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL ||
        __system_property_get("ro.build.version.sdk",sdk)<=0 || strcmp(sdk,"32")) {
        fputs("API32_SERVICE_POLICY_REQUIRES_GUEST_LOADER\n",stderr);_exit(125);
    }
#ifdef POCKET_FRAMEWORK_POLICY
    fputs("API32_FRAMEWORK_POLICY guest_selinux=disabled host_policy=inherited\n",stderr);
#else
    fputs("API32_SERVICE_POLICY guest_selinux=disabled host_policy=inherited\n",stderr);
#endif
}

int is_selinux_enabled(void) {return 0;}

// Match libselinux's documented disabled-policy path: restorecon is a no-op
// when there is no guest policy. Do not attempt to relabel host app files.
int selinux_android_restorecon(const char* path,unsigned int flags) {
    (void)path;(void)flags;return 0;
}
int selinux_android_restorecon_pkgdir(const char* path,const char* seinfo,uid_t uid,unsigned int flags) {
    (void)path;(void)seinfo;(void)uid;(void)flags;return 0;
}

// installd's exec workers (dex2oat/profman/dexoptanalyzer) are separate ART
// programs. The service's Binder and policy adapters belong to installd only;
// retaining them in LD_PRELOAD crosses the stock ART namespace boundary.
// Preserve unrelated preloads and every other environment entry.
#ifndef POCKET_FRAMEWORK_POLICY
int execv(const char* path,char* const argv[]) {
    extern char** environ;
    size_t count=0;while(environ[count])count++;
    char** env=calloc(count+1,sizeof(char*));
    const char* original=getenv("LD_PRELOAD");
    char* tokens=original?strdup(original):NULL;
    char* preload=original?calloc(strlen(original)+sizeof("LD_PRELOAD="),1):NULL;
    if(!env || (original && (!tokens || !preload))) {
        free(env);free(tokens);free(preload);errno=ENOMEM;return -1;
    }
    if(preload) {
        strcpy(preload,"LD_PRELOAD=");
        char* state=NULL;
        for(char* token=strtok_r(tokens,": \t",&state);token;token=strtok_r(NULL,": \t",&state)) {
            if(!strcmp(token,"/system/lib64/libpocket_binder_api32.so") ||
               !strcmp(token,"/system/lib64/libpocket_policy_api32.so"))continue;
            if(preload[sizeof("LD_PRELOAD=")-1])strcat(preload,":");
            strcat(preload,token);
        }
    }
    size_t next=0;
    for(size_t i=0;i<count;i++)if(strncmp(environ[i],"LD_PRELOAD=",sizeof("LD_PRELOAD=")-1))env[next++]=environ[i];
    if(preload && preload[sizeof("LD_PRELOAD=")-1])env[next++]=preload;
    execve(path,argv,env);
    int error=errno;free(env);free(tokens);free(preload);errno=error;return -1;
}
#endif
