// SPDX-License-Identifier: GPL-3.0-only
// ART trace transport and opt-in startup diagnostics in the private fixture.
#include <palette/palette.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>

typedef palette_status_t (*stock_writer)(const char*,size_t);
static stock_writer writer;
static pthread_once_t writer_once=PTHREAD_ONCE_INIT;
static void load_stock_writer(void) {
    void* library=dlopen("/system/lib64/libartpalette-system.so",RTLD_NOW|RTLD_LOCAL);
    if(library)writer=(stock_writer)dlsym(library,"PaletteWriteCrashThreadStacks");
}

palette_status_t PaletteWriteCrashThreadStacks(const char* data,size_t size) {
    char sdk[PROP_VALUE_MAX];__system_property_get("ro.build.version.sdk",sdk);
    if(syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL || !data || !size || size>8*1024*1024)
        return PALETTE_STATUS_FAILED_CHECK_LOG;
    if(strcmp(sdk,"32"))return PALETTE_STATUS_FAILED_CHECK_LOG;
    const char* tombstoned=getenv("POCKET_API32_USE_TOMBSTONED");
    if(tombstoned && !strcmp(tombstoned,"1")) {
        pthread_once(&writer_once,load_stock_writer);
        if(!writer || writer==PaletteWriteCrashThreadStacks)return PALETTE_STATUS_FAILED_CHECK_LOG;
        palette_status_t status=writer(data,size);
        fprintf(stderr,"API32_ART_TRACE_DELEGATED pid=%d bytes=%zu status=%d\n",getpid(),size,status);
        return status;
    }
    int directory=open("/tmp/art-traces",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(directory<0)return PALETTE_STATUS_FAILED_CHECK_LOG;
    struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now)){close(directory);return PALETTE_STATUS_FAILED_CHECK_LOG;}
    char name[96];snprintf(name,sizeof(name),"anr_%d_%" PRId64 "_%09ld",getpid(),(int64_t)now.tv_sec,now.tv_nsec);
    int fd=openat(directory,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);close(directory);
    if(fd<0)return PALETTE_STATUS_FAILED_CHECK_LOG;
    size_t at=0;
    while(at<size) {
        ssize_t count=write(fd,data+at,size-at);
        if(count<0 && errno==EINTR)continue;
        if(count<=0){close(fd);return PALETTE_STATUS_FAILED_CHECK_LOG;}
        at+=(size_t)count;
    }
    if(close(fd))return PALETTE_STATUS_FAILED_CHECK_LOG;
    fprintf(stderr,"API32_ART_TRACE_WRITTEN pid=%d path=/tmp/art-traces/%s bytes=%zu source=ART\n",getpid(),name,size);
    return PALETTE_STATUS_OK;
}
static void* watch_startup(void* unused) {
    (void)unused;
    for(unsigned sample=0;sample<12;sample++) {
        struct timespec remaining={10,0};
        while(nanosleep(&remaining,&remaining)<0 && errno==EINTR){}
        // Each process signals only itself; no debugger privileges or process
        // identity changes are required. ART supplies the actual stack text.
        kill(getpid(),SIGQUIT);
    }
    return NULL;
}
static int speed_header(const char* path,unsigned device_major,unsigned device_minor,unsigned long inode) {
    const char* visible=strstr(path,"/data/dalvik-cache/arm64/");
    if(!visible)visible=strstr(path,"/system_ext/priv-app/SystemUI/");
    if(!visible)visible=strstr(path,"/system/priv-app/SystemUI/");
    if(!visible)return 0;
    int fd=open(visible,O_RDONLY|O_CLOEXEC);if(fd<0)return 0;
    struct stat st;
    if(fstat(fd,&st) || (unsigned long)st.st_ino!=inode || major(st.st_dev)!=device_major || minor(st.st_dev)!=device_minor){close(fd);return 0;}
    char* header=malloc(131072);if(!header){close(fd);return 0;}
    size_t size=0;
    while(size<131072) {
        ssize_t n=read(fd,header+size,131072-size);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)break;
        size+=n;
    }
    close(fd);
    const char tag[]="compiler-filter\0speed\0";
    int matches=size>64 && !memcmp(header,"\177ELF",4) && memmem(header,size,tag,sizeof(tag)-1)!=NULL;
    free(header);return matches;
}
static void* observe_startup_code(void* unused) {
    (void)unused;pthread_setname_np(pthread_self(),"pocket-codeobs");
    struct timespec began;clock_gettime(CLOCK_MONOTONIC,&began);
    const unsigned delays[]={2,6,8};
    for(unsigned sample=0;sample<3;++sample) {
        struct timespec remaining={delays[sample],0},now;
        while(nanosleep(&remaining,&remaining)<0 && errno==EINTR){}
        clock_gettime(CLOCK_MONOTONIC,&now);
        int64_t elapsed=(now.tv_sec-began.tv_sec)*1000+(now.tv_nsec-began.tv_nsec)/1000000;
        FILE* maps=fopen("/proc/self/maps","re");int maps_ok=maps!=NULL;
        unsigned rows=0,executable=0,speed=0;char line[4096];
        if(maps) {
            while(fgets(line,sizeof(line),maps)) {
                if(!strstr(line,"SystemUI"))continue;
                unsigned long begin,end,offset,inode;unsigned device_major,device_minor;char permissions[5]={0};
                if(sscanf(line,"%lx-%lx %4s %lx %x:%x %lu",&begin,&end,permissions,&offset,&device_major,&device_minor,&inode)!=7)continue;
                if(rows<16)fprintf(stderr,"API32_STARTUP_CODE_MAP pid=%d sample=%u elapsed_ms=%" PRId64 " row=%s",getpid(),sample,elapsed,line);
                rows++;
                if(permissions[2]=='x') {
                    executable++;
                    char* path=strchr(line,'/');
                    if(path){path[strcspn(path,"\n")]=0;speed+=speed_header(path,device_major,device_minor,inode);}
                }
            }
            if(ferror(maps))maps_ok=0;
            fclose(maps);
        }
        unsigned long long cpu=0,wait=0,slices=0;
        FILE* stat=fopen("/proc/self/schedstat","re");int stat_ok=0;
        if(stat){stat_ok=fscanf(stat,"%llu %llu %llu",&cpu,&wait,&slices)==3;fclose(stat);}
        fprintf(stderr,"API32_STARTUP_CODE_SNAPSHOT pid=%d sample=%u elapsed_ms=%" PRId64 " maps_readable=%d systemui_rows=%u executable=%u verified_speed=%u main_schedstat=%d cpu_ns=%llu runnable_wait_ns=%llu slices=%llu\n",
            getpid(),sample,elapsed,maps_ok,rows,executable,speed,stat_ok,cpu,wait,slices);
    }
    return NULL;
}
void pocketStartStartupTrace32(void) {
    static pid_t started,code_started;
    const char* enabled=getenv("POCKET_API32_TRACE_STARTUP");
    const char* code=getenv("POCKET_API32_OBSERVE_STARTUP_CODE");
    int stack_enabled=enabled && !strcmp(enabled,"1"),code_enabled=code && !strcmp(code,"1");
    if(!stack_enabled && !code_enabled)return;
    if(syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL || access("/.pocket-api32-fixture",F_OK))return;
    // Only the three startup participants under investigation. Do not keep
    // sampling every guest application or create a thread in the zygote parent.
    char name[128]={0};int fd=open("/proc/self/cmdline",O_RDONLY|O_CLOEXEC);
    if(fd<0)return;
    ssize_t count=read(fd,name,sizeof(name)-1);close(fd);
    if(count<=0 || (strcmp(name,"system_server") &&
        strcmp(name,"com.android.networkstack.process") && strcmp(name,"com.android.systemui")))return;
    pthread_t thread;
    if(code_enabled && code_started!=getpid() && !strcmp(name,"com.android.systemui") &&
            !pthread_create(&thread,NULL,observe_startup_code,NULL)){code_started=getpid();pthread_detach(thread);}
    if(stack_enabled && started!=getpid() && !pthread_create(&thread,NULL,watch_startup,NULL)){
        started=getpid();pthread_detach(thread);
    }
}
