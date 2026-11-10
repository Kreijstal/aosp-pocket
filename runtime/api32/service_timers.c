// SPDX-License-Identifier: GPL-3.0-only
// Explicit rootless Bluetooth callback policy; actual timers remain in bionic.
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

typedef int (*timer_create_fn)(clockid_t,struct sigevent*,timer_t*);
typedef int (*set_scheduler_fn)(pid_t,int,const struct sched_param*);
static timer_create_fn stock_create;
static set_scheduler_fn stock_scheduler;
static pthread_once_t resolve_once=PTHREAD_ONCE_INIT;
static atomic_flag reported=ATOMIC_FLAG_INIT;
static atomic_flag scheduling_reported=ATOMIC_FLAG_INIT;
static void resolve_create(void) {
    stock_create=(timer_create_fn)dlsym(RTLD_NEXT,"timer_create");
    stock_scheduler=(set_scheduler_fn)dlsym(RTLD_NEXT,"sched_setscheduler");
}
static int rootless_bluetooth(void) {
    const char* enabled=getenv("POCKET_API32_ROOTLESS_BLUETOOTH_SCHEDULING");
    if(!enabled || strcmp(enabled,"1") || getuid()!=1002 ||
            syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL || access("/.pocket-api32-fixture",F_OK))return 0;
    char sdk[PROP_VALUE_MAX]={0};__system_property_get("ro.build.version.sdk",sdk);
    if(strcmp(sdk,"32"))return 0;
    char name[64]={0};int fd=open("/proc/self/cmdline",O_RDONLY|O_CLOEXEC);
    if(fd<0)return 0;
    ssize_t count=read(fd,name,sizeof(name)-1);close(fd);
    return count>0 && !strcmp(name,"com.android.bluetooth");
}
int timer_create(clockid_t clock,struct sigevent* event,timer_t* timer) {
    pthread_once(&resolve_once,resolve_create);
    if(!stock_create || stock_create==timer_create){errno=ENOSYS;return -1;}
    struct sigevent adjusted;pthread_attr_t attributes;int policy=0;struct sched_param priority={0};
    int changed=0;
    if(event && event->sigev_notify==SIGEV_THREAD && event->sigev_notify_attributes && rootless_bluetooth() &&
            !pthread_attr_getschedpolicy(event->sigev_notify_attributes,&policy) &&
            (policy==SCHED_FIFO || policy==SCHED_RR) &&
            !pthread_attr_getschedparam(event->sigev_notify_attributes,&priority) &&
            priority.sched_priority>=sched_get_priority_min(policy) &&
            priority.sched_priority<=sched_get_priority_max(policy)) {
        // Preserve stack/guard/detach settings and the caller's attributes.
        adjusted=*event;attributes=*(const pthread_attr_t*)event->sigev_notify_attributes;
        struct sched_param ordinary={.sched_priority=0};
        int error=pthread_attr_setschedpolicy(&attributes,SCHED_OTHER);
        if(!error)error=pthread_attr_setschedparam(&attributes,&ordinary);
        if(error){errno=error;return -1;}
        adjusted.sigev_notify_attributes=&attributes;event=&adjusted;changed=1;
    }
    // In particular, do not translate *_ALARM clocks or manufacture success:
    // CAP_WAKE_ALARM is absent and stock Bluetooth has a BOOTTIME fallback.
    int result=stock_create(clock,event,timer);int error=errno;
    if(!result && changed && !atomic_flag_test_and_set(&reported))
        fprintf(stderr,"API32_SERVICE_TIMER_THREAD_POLICY pid=%d uid=%u requested_policy=%d requested_priority=%d callback_policy=SCHED_OTHER clock=%d\n",
                getpid(),getuid(),policy,priority.sched_priority,clock);
    errno=error;return result;
}

int sched_setscheduler(pid_t tid,int policy,const struct sched_param* parameters) {
    pthread_once(&resolve_once,resolve_create);
    if(!stock_scheduler || stock_scheduler==sched_setscheduler){errno=ENOSYS;return -1;}
    int changed=0;struct sched_param ordinary={.sched_priority=0};
    if(parameters && (policy==SCHED_FIFO || policy==SCHED_RR) && rootless_bluetooth() &&
            parameters->sched_priority>=sched_get_priority_min(policy) &&
            parameters->sched_priority<=sched_get_priority_max(policy)) {
        // A service may adapt only one of its own live threads. All other
        // requests retain bionic/kernel permission and argument validation.
        char path[80];snprintf(path,sizeof(path),"/proc/self/task/%d",tid?tid:gettid());
        if(tid>=0 && !access(path,F_OK))changed=1;
    }
    int result=stock_scheduler(tid,changed?SCHED_OTHER:policy,changed?&ordinary:parameters);int error=errno;
    if(!result && changed && !atomic_flag_test_and_set(&scheduling_reported))
        fprintf(stderr,"API32_SERVICE_SCHEDULER_POLICY pid=%d uid=%u tid=%d requested_policy=%d actual_policy=SCHED_OTHER\n",
                getpid(),getuid(),tid,policy);
    errno=error;return result;
}
