// SPDX-License-Identifier: GPL-3.0-only
// Software-renderer workers must not inherit SurfaceFlinger's display priority.
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <unistd.h>

typedef void* (*Start)(void*);
typedef int (*Create)(pthread_t*,const pthread_attr_t*,Start,void*);
static Create real_create;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static atomic_uint reported;
static void resolve(void) {real_create=(Create)dlsym(RTLD_NEXT,"pthread_create");}
static int software_compositor(void) {
    char sdk[PROP_VALUE_MAX],command[256];
    if(getuid()!=1000 || syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL)return 0;
    __system_property_get("ro.build.version.sdk",sdk);if(strcmp(sdk,"32"))return 0;
    int fd=open("/proc/self/cmdline",O_RDONLY|O_CLOEXEC);if(fd<0)return 0;
    ssize_t n=read(fd,command,sizeof(command)-1);close(fd);if(n<=0)return 0;
    command[n]=0;const char* name=strrchr(command,'/');
    return !strcmp(name?name+1:command,"surfaceflinger");
}
struct Worker {Start start;void* argument;const char* library;};
static void* worker(void* opaque) {
    int original_errno=errno;struct Worker context=*(struct Worker*)opaque;free(opaque);
    errno=0;int before=getpriority(PRIO_PROCESS,0);int error=errno;
    if(!error && before<0 && setpriority(PRIO_PROCESS,0,0))error=errno;
    errno=0;int after=getpriority(PRIO_PROCESS,0);if(errno && !error)error=errno;
    if(atomic_fetch_add_explicit(&reported,1,memory_order_relaxed)<32)
        fprintf(stderr,"POCKET_RENDER_WORKER_NICE pid=%d tid=%ld before=%d after=%d error=%d library=%s\n",
            getpid(),syscall(__NR_gettid),before,after,error,context.library);
    errno=original_errno;return context.start(context.argument);
}
int pthread_create(pthread_t* thread,const pthread_attr_t* attributes,Start start,void* argument) {
    int original_errno=errno;pthread_once(&once,resolve);
    if(!real_create){errno=original_errno;return ENOSYS;}
    const char* library=NULL;Dl_info image;
    if(software_compositor() && dladdr((void*)start,&image) && image.dli_fname) {
        const char* name=strrchr(image.dli_fname,'/');name=name?name+1:image.dli_fname;
        if(!strcmp(name,"libGLESv2_swiftshader.so"))library="GLESv2";
        else if(!strcmp(name,"libEGL_swiftshader.so"))library="EGL";
    }
    if(!library){errno=original_errno;return real_create(thread,attributes,start,argument);}
    struct Worker* context=malloc(sizeof(*context));
    if(!context){errno=original_errno;return ENOMEM;}
    *context=(struct Worker){start,argument,library};errno=original_errno;
    int result=real_create(thread,attributes,worker,context);int actual_errno=errno;
    if(result)free(context);
    errno=actual_errno;return result;
}
