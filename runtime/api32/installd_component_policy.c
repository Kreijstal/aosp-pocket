// SPDX-License-Identifier: GPL-3.0-only
// Only installd source calls these explicitly named hooks. They do not
// interpose policy or exec functions belonging to the other core components.
#include <selinux/android.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include "installd_worker_env.h"

static int guest32(void) {
    char sdk[PROP_VALUE_MAX];
    return syscall(__NR_getpid,0x504c1001UL)==0x504c1002UL &&
        __system_property_get("ro.build.version.sdk",sdk)>0 && !strcmp(sdk,"32");
}
int pocketInstalldSelinuxEnabled32(void) {return 0;}
int pocketInstalldRestorecon32(const char* path,unsigned int flags) {
    (void)path;(void)flags;
    if(!guest32()){errno=EPERM;return -1;}
    return 0;
}
int pocketInstalldRestoreconPkgdir32(const char* path,const char* seinfo,uid_t uid,unsigned int flags) {
    (void)path;(void)seinfo;(void)uid;(void)flags;
    if(!guest32()){errno=EPERM;return -1;}
    return 0;
}
int pocketInstalldExecv32(const char* path,char* const argv[]) {
    if(!guest32()){errno=EPERM;return -1;}
    extern char** environ;
    struct PocketInstalldWorkerEnv env;
    if(pocketInstalldBuildWorkerEnv(environ,&env)<0)return -1;
    execve(path,argv,env.values);
    int error=errno;pocketInstalldFreeWorkerEnv(&env);errno=error;return -1;
}
