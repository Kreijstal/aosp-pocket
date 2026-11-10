// SPDX-License-Identifier: GPL-3.0-only
// Exercise the environment across a real fork/exec while the parent observes
// its own environment. No guest service adapter may reach the child loader.
#define _GNU_SOURCE
#include "../../api32/installd_worker_env.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static atomic_int running;
static const char* parent_preload;
static void* observe(void* unused) {
    (void)unused;
    while(atomic_load(&running)) {
        const char* current=getenv("LD_PRELOAD");
        assert(parent_preload ? current && !strcmp(current,parent_preload) : !current);
        assert(!strcmp(getenv("POCKET_WORKER_KEEP"),"a=b c"));
    }
    return NULL;
}
static void check(const char* preload,const char* expected) {
    if(preload)assert(!setenv("LD_PRELOAD",preload,1));
    else assert(!unsetenv("LD_PRELOAD"));
    parent_preload=preload;
    atomic_store(&running,1);
    pthread_t observer;assert(!pthread_create(&observer,NULL,observe,NULL));
    pid_t pid=fork();assert(pid>=0);
    if(!pid) {
        extern char** environ;
        struct PocketInstalldWorkerEnv env;
        if(pocketInstalldBuildWorkerEnv(environ,&env)<0)_exit(90);
        char* args[]={"installd-worker-env","child",(char*)expected,NULL};
        execve("/proc/self/exe",args,env.values);
        _exit(91);
    }
    int status;
    while(waitpid(pid,&status,0)<0)assert(errno==EINTR);
    atomic_store(&running,0);assert(!pthread_join(observer,NULL));
    assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
    const char* current=getenv("LD_PRELOAD");
    assert(preload ? current && !strcmp(current,preload) : !current);
}
int main(int argc,char** argv) {
    if(argc==3 && !strcmp(argv[1],"child")) {
        const char* preload=getenv("LD_PRELOAD");
        assert(*argv[2] ? preload && !strcmp(preload,argv[2]) : !preload);
        assert(!strcmp(getenv("POCKET_WORKER_KEEP"),"a=b c"));
        return 0;
    }
    assert(argc==1);
    assert(!setenv("POCKET_WORKER_KEEP","a=b c",1));
    check("/system/lib64/libpocket_binder_api32.so:"
          "/system/lib64/libpocket_hidl_api32.so\t"
          "/system/lib64/libpocket_policy_api32.so","");
    check("libm.so.6 /system/lib64/libpocket_hidl_api32.so:"
          "/system/lib64/libpocket_binder_api32.so:"
          "/system/lib64/libpocket_policy_api32.so:libc.so.6","libm.so.6:libc.so.6");
    check(NULL,"");
    check("","");
    check("libm.so.6","libm.so.6");
    puts("PASS_INSTALLD_WORKER_EXEC_ENV parent_unchanged=1 unrelated_preserved=1 daemon_preloads_absent=1 cases=5");
    return 0;
}
