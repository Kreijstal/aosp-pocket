// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/system_properties.h>
#include <sys/xattr.h>
#include <sys/inotify.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"BOOT_PROBE_FAIL line=%d errno=%d %s\n",__LINE__,errno,strerror(errno));return 1; } } while(0)
static int prepare_data(uid_t kernel_uid) {
    struct Directory { const char* path; mode_t mode; uid_t uid; gid_t gid; };
    const struct Directory dirs[]={
        {"/data/app",0771,1000,1000},{"/data/app-private",0771,1000,1000},
        {"/data/cache",0770,1000,2001},
        {"/data/app-lib",0771,1000,1000},{"/data/app-ephemeral",0771,1000,1000},
        {"/data/app-asec",0700,0,0},{"/data/property",0700,0,0},
        {"/data/system",0775,1000,1000},{"/data/system/users",0775,1000,1000},
        {"/data/system/users/0",0770,1000,1000},{"/data/system_ce",0770,1000,1000},
        {"/data/system_ce/0",0770,1000,1000},{"/data/system_de",0770,1000,1000},
        {"/data/system_de/0",0770,1000,1000},{"/data/misc",01771,1000,9998},
        {"/data/misc/user",0710,0,0},{"/data/misc/user/0",0710,0,0},
        {"/data/misc/profiles",0771,1000,1000},{"/data/misc/profiles/cur",0771,1000,1000},
        {"/data/misc/profiles/cur/0",0771,1000,1000},{"/data/misc/profiles/ref",0771,1000,1000},
        {"/data/misc/gatekeeper",0700,1000,1000},{"/data/misc/keychain",0771,1000,1000},{"/data/misc/keystore",0700,1017,1017},
        {"/data/user",0711,1000,1000},{"/data/user/0",0771,1000,1000},
        {"/data/user_de",0711,1000,1000},{"/data/user_de/0",0771,1000,1000},
        {"/data/media",0770,1023,1023},{"/data/media/0",0770,1023,1023},
        {"/data/local",0751,0,0},{"/data/local/tmp",0771,2000,2000},
        {"/data/resource-cache",0771,1000,1000},{"/data/dalvik-cache",0771,0,0},
        {"/data/dalvik-cache/arm64",0771,0,0},{"/data/anr",0775,1000,1000},
        {"/data/tombstones",0771,1000,1000},{"/data/misc_de",01771,1000,9998},
        {"/data/misc_de/0",01771,1000,9998},{"/data/misc_ce",01771,1000,9998},
        {"/data/misc_ce/0",01771,1000,9998},
    };
    CHECK(getuid()==0);
    char canonical[PATH_MAX],descriptor[64];
    CHECK(realpath("/data",canonical) && !strcmp(canonical,"/data"));
    int data=open("/data",O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(data>=0);
    CHECK(fchdir(data)==0 && getcwd(canonical,sizeof(canonical)) && !strcmp(canonical,"/data"));
    snprintf(descriptor,sizeof(descriptor),"/proc/self/fd/%d",data);
    ssize_t visible=readlink(descriptor,canonical,sizeof(canonical)-1);CHECK(visible>0);canonical[visible]=0;
    CHECK(!strcmp(canonical,"/data") && chdir("/")==0);close(data);
    puts("DIRECT_DATA_VIEW_PASS canonical_cwd_fd=/data mount_semantics=virtual");
    CHECK(mkdir("/dev/input",0755)==0 || errno==EEXIST);
    for (size_t i=0;i<sizeof(dirs)/sizeof(*dirs);++i) {
        const struct Directory* d=&dirs[i];
        CHECK(mkdir(d->path,d->mode)==0 || errno==EEXIST);
        CHECK(chmod(d->path,d->mode)==0 && chown(d->path,d->uid,d->gid)==0);
        struct stat st;CHECK(stat(d->path,&st)==0 && S_ISDIR(st.st_mode));
        CHECK(st.st_uid==d->uid && st.st_gid==d->gid && (st.st_mode&07777)==d->mode);
        int fd=open(d->path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(fd>=0);
        CHECK(syscall(SYS_fstat,fd,&st)==0 && st.st_uid==kernel_uid);close(fd);
    }
    CHECK(symlink("user/0","/data/data")==0 || errno==EEXIST);
    CHECK(mkdir("/storage",0755)==0 || errno==EEXIST);
    CHECK(mkdir("/storage/self",0755)==0 || errno==EEXIST);
    CHECK(symlink("/data/media","/storage/emulated")==0 || errno==EEXIST);
    CHECK(symlink("../emulated/0","/storage/self/primary")==0 || errno==EEXIST);
    CHECK(symlink("/storage/emulated/0","/sdcard")==0 || errno==EEXIST);
    CHECK(symlink("/data/cache","/cache")==0 || errno==EEXIST);
    struct statvfs cache;CHECK(statvfs("/data/cache",&cache)==0 && cache.f_bsize>0);
    CHECK(__system_property_set("pocket.data.ready","1")==0);
    printf("DIRECT_DATA_PREPARED directories=%zu kernel_uid=%d owner_metadata=passed\n",sizeof(dirs)/sizeof(*dirs),kernel_uid);
    return 0;
}
static const gid_t groups[]={1001,1002,1003,1004,1005,1006,1007,1008,1009,1010,1018,1021,1023,1024,1032,1065,3001,3002,3003,3006,3007,3009,3010};
static int check_groups(void) {
    gid_t actual[64]; int n=getgroups(64,actual);
    CHECK(n==(int)(sizeof(groups)/sizeof(*groups)) && !memcmp(actual,groups,sizeof(groups)));
    CHECK(getgroups(0,NULL)==n);
    errno=0;CHECK(getgroups(1,actual)==-1 && errno==EINVAL);
    return 0;
}
int main(int argc,char** argv) {
    setbuf(stdout,NULL);
    uid_t kernel_uid=syscall(SYS_getuid);
    gid_t kernel_groups[64], after[64];
    long count=syscall(SYS_getgroups,64,kernel_groups);
    CHECK(kernel_uid>=10000 && count>=0);
    if (argc==2 && !strcmp(argv[1],"prepare-data")) return prepare_data(kernel_uid);
    if (argc==2 && !strcmp(argv[1],"app")) {
        gid_t actual[2];CHECK(getuid()==39999 && getgid()==39999);
        CHECK(getgroups(2,actual)==2 && actual[0]==1000 && actual[1]==3010);
        CHECK(syscall(SYS_getgroups,64,after)==count && !memcmp(after,kernel_groups,count*sizeof(gid_t)));
        puts("DIRECT_APP_GROUPS_PASS guest_uid=39999 groups=1000,3010 kernel_groups_unchanged=1");return 0;
    }
    if (argc==2 && !strcmp(argv[1],"exec")) {
        CHECK(getuid()==1000 && !check_groups());
        errno=0;CHECK(setgroups(0,NULL)==-1 && errno==EPERM);
        errno=0;CHECK(setuid(0)==-1 && errno==EPERM);
        puts("DIRECT_BOOT_EXEC_PASS guest_uid=1000 supplementary_groups=23 regain_root=denied");
        return 0;
    }
    CHECK(argc==1 && getuid()==0);
    int (*eventInit)(void)=dlsym(RTLD_DEFAULT,"uevent_init");
    int (*eventNext)(char*,int)=dlsym(RTLD_DEFAULT,"uevent_next_event");
    int (*eventSend)(const char*,int)=dlsym(RTLD_DEFAULT,"pocket_uevent_inject");
    CHECK(eventInit && eventNext && eventSend && eventInit());
    const char eventRecord[]="add@/devices/pocket-test\0ACTION=add\0DEVPATH=/devices/pocket-test\0";
    CHECK(eventSend(eventRecord,sizeof(eventRecord))==sizeof(eventRecord));
    char eventData[512];CHECK(eventNext(eventData,sizeof(eventData))==sizeof(eventRecord));
    CHECK(!memcmp(eventData,eventRecord,sizeof(eventRecord)));
    puts("DIRECT_UEVENT_PASS private_datagram_delivery=1 host_netlink=unused source=synthetic_test");
    int notify=inotify_init1(IN_CLOEXEC|IN_NONBLOCK);CHECK(notify>=0);
    CHECK(mkdir("/data/boot-notify",0700)==0 || errno==EEXIST);
    int watch=inotify_add_watch(notify,"/data/boot-notify",IN_CREATE|IN_ONLYDIR);CHECK(watch>=0);
    int created=open("/data/boot-notify/event",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);CHECK(created>=0);close(created);
    char events[4096];ssize_t received=read(notify,events,sizeof(events));CHECK(received>=(ssize_t)sizeof(struct inotify_event));
    struct inotify_event* event=(struct inotify_event*)events;
    CHECK(event->wd==watch && (event->mask&IN_CREATE) && event->len && !strcmp(event->name,"event"));
    CHECK(inotify_rm_watch(notify,watch)==0);close(notify);
    CHECK(unlink("/data/boot-notify/event")==0 && rmdir("/data/boot-notify")==0);
    puts("DIRECT_INOTIFY_PASS private_directory_create_event=passed");
    for (size_t i=0;i<2;++i) {
        int timer=timerfd_create(i?CLOCK_BOOTTIME_ALARM:CLOCK_REALTIME_ALARM,TFD_CLOEXEC);CHECK(timer>=0);
        struct itimerspec deadline={0};deadline.it_value.tv_nsec=20000000;
        CHECK(timerfd_settime(timer,0,&deadline,NULL)==0);
        struct pollfd ready={timer,POLLIN,0};CHECK(poll(&ready,1,2000)==1 && (ready.revents&POLLIN));
        uint64_t expirations=0;CHECK(read(timer,&expirations,sizeof(expirations))==sizeof(expirations) && expirations>0);close(timer);
    }
    struct timezone zone={0};errno=0;CHECK(settimeofday(NULL,&zone)==-1 && errno==EPERM);
    puts("DIRECT_TIMER_PASS realtime_boottime_expiration=passed host_time_change=denied host_wakeup=unsupported");
    errno=0;CHECK(setgroups(65,groups)==-1 && errno==EINVAL);
    CHECK(setgroups(sizeof(groups)/sizeof(*groups),groups)==0 && !check_groups());
    CHECK(syscall(SYS_getgroups,64,after)==count && !memcmp(after,kernel_groups,count*sizeof(gid_t)));
    int root_file=open("/data/root-owner-probe",O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(root_file>=0);
    struct stat root_stat;CHECK(fchown(root_file,1234,2345)==0 && fstat(root_file,&root_stat)==0 && root_stat.st_uid==1234 && root_stat.st_gid==2345);
    CHECK(fchown(root_file,0,0)==0);close(root_file);
    CHECK(setresgid(1000,1000,1000)==0 && setresuid(1000,1000,1000)==0);
    errno=0;CHECK(chown("/data/root-owner-probe",(uid_t)-1,1000)==-1 && errno==EPERM);
    CHECK(unlink("/data/root-owner-probe")==0);
    int fd=open("/data/boot-owner-probe",O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(fd>=0);
    struct stat st;CHECK(fstat(fd,&st)==0 && st.st_uid==1000 && st.st_gid==1000);
    CHECK(syscall(SYS_fstat,fd,&st)==0 && st.st_uid==kernel_uid);
    errno=0;CHECK(fchown(fd,1001,(gid_t)-1)==-1 && errno==EPERM);
    CHECK(fchown(fd,1000,1000)==0);
    CHECK(fchown(fd,(uid_t)-1,1032)==0 && fstat(fd,&st)==0 && st.st_gid==1032);
    CHECK(chown("/data/boot-owner-probe",(uid_t)-1,3010)==0 && fstat(fd,&st)==0 && st.st_gid==3010);
    errno=0;CHECK(fchown(fd,(uid_t)-1,54321)==-1 && errno==EPERM);
    const char* attr="user.pocket.test";char value[16]={0},list[256]={0};
    CHECK(setxattr("/data/boot-owner-probe",attr,"one",3,XATTR_CREATE)==0);
    CHECK(getxattr("/data/boot-owner-probe",attr,NULL,0)==3);
    CHECK(fgetxattr(fd,attr,value,sizeof(value))==3 && !memcmp(value,"one",3));
    CHECK(lsetxattr("/data/boot-owner-probe",attr,"two",3,XATTR_REPLACE)==0);
    CHECK(lgetxattr("/data/boot-owner-probe",attr,value,sizeof(value))==3 && !memcmp(value,"two",3));
    CHECK(listxattr("/data/boot-owner-probe",list,sizeof(list))>0);
    CHECK(llistxattr("/data/boot-owner-probe",list,sizeof(list))>0);
    CHECK(removexattr("/data/boot-owner-probe",attr)==0);
    CHECK(setxattr("/data/boot-owner-probe",attr,"three",5,0)==0);
    CHECK(lremovexattr("/data/boot-owner-probe",attr)==0);
    errno=0;CHECK(getxattr("/data/boot-owner-probe",attr,value,sizeof(value))==-1 && errno==ENODATA);
    puts("DIRECT_XATTR_PASS path_fd_same_inode=1 create_replace_list_remove=passed");close(fd);
    CHECK(unlink("/data/boot-owner-probe")==0);
    pid_t child=fork();CHECK(child>=0);
    if (!child) { char* args[]={argv[0],"exec",NULL};execv(argv[0],args);_exit(127); }
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(syscall(SYS_getuid)==kernel_uid);
    printf("DIRECT_BOOT_PROFILE_PASS kernel_uid=%d guest_uid=%d groups=23 kernel_groups_unchanged=1 owner_metadata=passed fork_exec=passed\n",kernel_uid,getuid());
    return 0;
}
