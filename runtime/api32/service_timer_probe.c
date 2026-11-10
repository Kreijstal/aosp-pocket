// SPDX-License-Identifier: GPL-3.0-only
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(ok) do {if(!(ok)){fprintf(stderr,"API32_SERVICE_TIMER_FAIL line=%d errno=%d\n",__LINE__,errno);exit(1);}}while(0)
typedef int (*create_fn)(clockid_t,struct sigevent*,timer_t*);
typedef struct {pthread_mutex_t mutex;pthread_cond_t condition;unsigned calls;int policy,priority;pid_t tid;} State;
static State states[3];
static void callback(union sigval value) {
    State* state=value.sival_ptr;struct sched_param param={0};int policy=-1;
    CHECK(!pthread_getschedparam(pthread_self(),&policy,&param));
    pthread_mutex_lock(&state->mutex);state->policy=policy;state->priority=param.sched_priority;
    state->tid=gettid();++state->calls;pthread_cond_signal(&state->condition);pthread_mutex_unlock(&state->mutex);
}
static struct sigevent event_for(pthread_attr_t* attributes,State* state) {
    struct sigevent event={0};event.sigev_notify=SIGEV_THREAD;event.sigev_notify_function=callback;
    event.sigev_notify_attributes=attributes;event.sigev_value.sival_ptr=state;return event;
}
static pthread_attr_t attributes_for(int policy) {
    pthread_attr_t attributes;CHECK(!pthread_attr_init(&attributes));
    CHECK(!pthread_attr_setschedpolicy(&attributes,policy));
    struct sched_param priority={.sched_priority=policy==SCHED_OTHER?0:1};
    CHECK(!pthread_attr_setschedparam(&attributes,&priority));return attributes;
}
static void denied(create_fn create,clockid_t clock,pthread_attr_t* attributes) {
    struct sigevent event=event_for(attributes,&states[2]);timer_t timer;
    errno=0;int result=create(clock,&event,&timer);int error=errno;
    if(!result)timer_delete(timer);
    CHECK(result==-1 && error==EPERM);
}
static void denied_scheduler(pid_t tid) {
    struct sched_param priority={.sched_priority=1};errno=0;
    int result=sched_setscheduler(tid,SCHED_FIFO,&priority);int error=errno;
    CHECK(result==-1 && error==EPERM);
}
static unsigned exercise(create_fn create,pthread_attr_t* attributes,State* state) {
    CHECK(!pthread_mutex_init(&state->mutex,NULL));CHECK(!pthread_cond_init(&state->condition,NULL));
    struct sigevent event=event_for(attributes,state);timer_t timer;
    CHECK(!create(CLOCK_BOOTTIME,&event,&timer));
    struct itimerspec arm={0};arm.it_value.tv_nsec=20000000;arm.it_interval.tv_nsec=20000000;
    CHECK(!timer_settime(timer,0,&arm,NULL));
    struct timespec until;CHECK(!clock_gettime(CLOCK_REALTIME,&until));until.tv_sec+=2;
    pthread_mutex_lock(&state->mutex);
    while(state->calls<2) {int result=pthread_cond_timedwait(&state->condition,&state->mutex,&until);CHECK(!result);}
    CHECK(state->policy==SCHED_OTHER && state->priority==0 && state->tid!=gettid());
    unsigned calls=state->calls;pid_t thread=state->tid;pthread_mutex_unlock(&state->mutex);
    if(create==timer_create) {
        struct sched_param realtime={.sched_priority=1};
        CHECK(!sched_setscheduler(thread,SCHED_FIFO,&realtime));
        CHECK(sched_getscheduler(thread)==SCHED_OTHER);
        struct sched_param actual={0};CHECK(!sched_getparam(thread,&actual) && actual.sched_priority==0);
    }
    memset(&arm,0,sizeof(arm));CHECK(!timer_settime(timer,0,&arm,NULL));CHECK(!timer_delete(timer));
    return calls;
}
int main(int argc,char** argv) {
    char sdk[PROP_VALUE_MAX]={0};__system_property_get("ro.build.version.sdk",sdk);
    CHECK(!strcmp(sdk,"32") && syscall(__NR_getpid,0x504c1001UL)==0x504c1002UL);
    pthread_attr_t realtime=attributes_for(SCHED_FIFO);
    if(argc==1) {
        CHECK(getuid()==0);
        pid_t child=fork();CHECK(child>=0);
        if(!child) {
            CHECK(!setgid(39999) && !setuid(39999));
            char* args[]={"com.android.bluetooth","denied",NULL};execv("/system/bin/pocket-service-timer-probe-api32",args);CHECK(0);
        }
        int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
        CHECK(!setgid(1002) && !setuid(1002));
        denied(timer_create,CLOCK_BOOTTIME,&realtime);denied_scheduler(0);
        puts("API32_SERVICE_TIMER_NAME_SCOPE_PASS uid=1002 process=probe realtime=denied");fflush(stdout);
        char* args[]={"com.android.bluetooth","check",NULL};execv("/system/bin/pocket-service-timer-probe-api32",args);CHECK(0);
    }
    CHECK(argc==2);
    if(!strcmp(argv[1],"denied")) {
        CHECK(getuid()==39999);denied(timer_create,CLOCK_BOOTTIME,&realtime);denied_scheduler(0);
        puts("API32_SERVICE_TIMER_UID_SCOPE_PASS uid=39999 process=com.android.bluetooth realtime=denied");return 0;
    }
    CHECK(!strcmp(argv[1],"check") && getuid()==1002);
    void* libc=dlopen("libc.so",RTLD_NOW|RTLD_LOCAL);CHECK(libc);
    create_fn stock=(create_fn)dlsym(libc,"timer_create");CHECK(stock && stock!=timer_create);
    // Isolate the actual stock error before applying the scoped policy.
    // Stock timer_create internally calls the public sched_setscheduler too.
    CHECK(!setenv("POCKET_API32_ROOTLESS_BLUETOOTH_SCHEDULING","0",1));
    denied(stock,CLOCK_BOOTTIME,&realtime);
    pthread_attr_t ordinary=attributes_for(SCHED_OTHER);
    unsigned normal=exercise(stock,&ordinary,&states[0]);
    CHECK(!setenv("POCKET_API32_ROOTLESS_BLUETOOTH_SCHEDULING","1",1));
    unsigned adapted=exercise(timer_create,&realtime,&states[1]);
    int original;struct sched_param original_priority={0};
    CHECK(!pthread_attr_getschedpolicy(&realtime,&original) && original==SCHED_FIFO);
    CHECK(!pthread_attr_getschedparam(&realtime,&original_priority) && original_priority.sched_priority==1);
    // Actual kernel wake permission remains denied with either policy.
    denied(stock,CLOCK_BOOTTIME_ALARM,&ordinary);denied(timer_create,CLOCK_BOOTTIME_ALARM,&realtime);
    typedef int (*set_fn)(pid_t,int,const struct sched_param*);
    set_fn stock_set=(set_fn)dlsym(libc,"sched_setscheduler");CHECK(stock_set && stock_set!=sched_setscheduler);
    struct sched_param priority={.sched_priority=1};errno=0;
    CHECK(stock_set(0,SCHED_FIFO,&priority)==-1 && errno==EPERM);
    CHECK(!sched_setscheduler(0,SCHED_FIFO,&priority));CHECK(sched_getscheduler(0)==SCHED_OTHER);
    struct sched_param actual={0};CHECK(!sched_getparam(0,&actual) && actual.sched_priority==0);
    // Do not apply a local-thread policy to another live process, even when
    // its kernel and virtual UIDs happen to match ours.
    int hold[2];CHECK(!pipe(hold));pid_t other=fork();CHECK(other>=0);
    if(!other){close(hold[1]);char value;while(read(hold[0],&value,1)<0 && errno==EINTR){};_exit(0);}
    close(hold[0]);denied_scheduler(other);close(hold[1]);int status;
    CHECK(waitpid(other,&status,0)==other && WIFEXITED(status) && !WEXITSTATUS(status));
    puts("API32_SERVICE_SCHEDULER_PASS uid=1002 raw_realtime=denied local_thread=adapted distinct_thread=verified actual_policy=SCHED_OTHER actual_priority=0 other_process=unchanged");
    printf("API32_SERVICE_TIMER_PASS uid=1002 raw_realtime_errno=%d normal_callbacks=%u adapted_callbacks=%u callback_policy=SCHED_OTHER original_attributes=unchanged kernel_wake_alarm=denied timer_backend=stock_bionic\n",EPERM,normal,adapted);
    return 0;
}
