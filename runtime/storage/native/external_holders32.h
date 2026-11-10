// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/stat.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <time.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

static uint64_t holder_clock32(void) {
    struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))return 0;
    return (uint64_t)now.tv_sec*1000000000ULL+(uint64_t)now.tv_nsec;
}
static void holder_slow32(const char* phase,pid_t pid,const char* descriptor,int cached_stat,uint64_t began) {
    uint64_t now=holder_clock32();if(!began || now<began || now-began<100000000ULL)return;
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO,"PocketHolders32","phase=%s pid=%d fd=%s cached_stat=%d elapsed_ms=%llu",phase,pid,descriptor,cached_stat,(unsigned long long)((now-began)/1000000ULL));
#else
    fprintf(stderr,"HOLDER_SLOW32 phase=%s pid=%d fd=%s cached_stat=%d elapsed_ms=%llu\n",phase,pid,descriptor,cached_stat,(unsigned long long)((now-began)/1000000ULL));
#endif
}

// Host-side equivalent of vold KillProcessesUsingPath. Kernel device/inode
// evidence identifies holders; an owned profile identity and start time scope
// candidates. Only a verified pidfd is signalled, never a recycled numeric PID.
struct PocketHolderIdentity32 {uint32_t magic,pid,uid,gid;uint64_t start;};
_Static_assert(sizeof(struct PocketHolderIdentity32)==24,"Guest identity ABI");
static int holder_number32(const char* name,pid_t* pid) {
    if(!name || *name<'1' || *name>'9')return 0;
    char* end=NULL;errno=0;long value=strtol(name,&end,10);
    if(errno || !end || *end || value<=1 || value>INT_MAX)return 0;
    *pid=(pid_t)value;return 1;
}
static uint64_t holder_start32(int process) {
    int fd=openat(process,"stat",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return 0;
    char bytes[4096];ssize_t count=read(fd,bytes,sizeof(bytes)-1);close(fd);if(count<=0)return 0;bytes[count]=0;
    char* field=strrchr(bytes,')');if(!field)return 0;field++;
    for(unsigned n=3;n<=22;n++) {
        while(*field==' ')field++;
        if(n==22){char* end=NULL;errno=0;unsigned long long start=strtoull(field,&end,10);return !errno && end!=field && (*end==' ' || *end=='\n' || !*end)?start:0;}
        while(*field && *field!=' ')field++;
    }
    return 0;
}
static int holder_fd32(int directory,const char* name,dev_t device,int cached_stat) {
    if(cached_stat) {
#ifdef __NR_statx
        // Only immutable identity is needed. A synchronizing stat can call
        // back into the very FUSE service being unmounted, or into another
        // user's filesystem, while we scan unrelated process descriptors.
        struct statx st;memset(&st,0,sizeof(st));
        if(syscall(__NR_statx,directory,name,AT_STATX_DONT_SYNC,STATX_TYPE|STATX_INO,&st))return -errno;
        if((st.stx_mask&(STATX_TYPE|STATX_INO))!=(STATX_TYPE|STATX_INO))return -EPROTO;
        return S_ISREG(st.stx_mode) && st.stx_dev_major==major(device) && st.stx_dev_minor==minor(device) && st.stx_ino>=3;
#else
        return -ENOSYS;
#endif
    }
    // Pre-API30 hosts retain their existing syscall compatibility. Never
    // invoke a newer syscall through an older Android app seccomp policy.
    struct stat st;if(fstatat(directory,name,&st,0))return -errno;
    return S_ISREG(st.st_mode) && st.st_dev==device && st.st_ino>=3;
}
static int holder_uses32(int process,dev_t device,int cached_stat,pid_t pid) {
    int descriptor=openat(process,"fd",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(descriptor<0)return errno==ENOENT?0:-errno;
    DIR* fds=fdopendir(descriptor);if(!fds){int error=-errno;close(descriptor);return error;}
    int found=0;struct dirent* entry;
    for(;;) {
        errno=0;entry=readdir(fds);if(!entry){if(errno)found=-errno;break;}
        char* end=NULL;errno=0;strtoul(entry->d_name,&end,10);if(errno || end==entry->d_name || *end)continue;
        uint64_t began=holder_clock32();int match=holder_fd32(descriptor,entry->d_name,device,cached_stat);
        holder_slow32("fd_identity",pid,entry->d_name,cached_stat,began);
        if(match>0){found=1;break;}
        if(match<0 && match!=-ENOENT && match!=-EBADF){found=match;break;}
    }
    closedir(fds);if(found)return found;
    uint64_t began=holder_clock32();int fd=openat(process,"maps",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return errno==ENOENT?0:-errno;
    FILE* maps=fdopen(fd,"r");if(!maps){int error=-errno;close(fd);return error;}
    char* line=NULL;size_t size=0;unsigned lines=0;
    while(getline(&line,&size,maps)>=0) {
        unsigned dev_major,dev_minor;unsigned long long inode;
        if(++lines>262144){found=-E2BIG;break;}
        if(sscanf(line,"%*s %*s %*s %x:%x %llu",&dev_major,&dev_minor,&inode)==3
                && inode>=3 && dev_major==major(device) && dev_minor==minor(device)){found=1;break;}
    }
    if(!found && ferror(maps))found=-EIO;
    free(line);fclose(maps);holder_slow32("maps",pid,"-",cached_stat,began);return found;
}
static int pocket_retire_holders_scan32(const char* root,dev_t device,pid_t* retired,size_t capacity,int cached_stat,int prefer_proc) {
    if(!root || !device || !retired || !capacity || capacity>4096)return -EINVAL;
#if !defined(__NR_pidfd_open) || !defined(__NR_pidfd_send_signal)
    return -ENOSYS;
#else
    uint64_t total_began=holder_clock32();unsigned entries=0,records=0,ordinary=0,live=0;
    int rootfd=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(rootfd<0)return -errno;
    int ids=openat(rootfd,".pocket-identity",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(rootfd);if(ids<0)return -errno;
    // Identity files survive guest boots. Enumerating them makes teardown
    // grow with every historical process. The host can normally enumerate
    // its own UID's live processes through proc even with hidepid enabled.
    // Preserve the older discovery path if an Android policy denies this
    // directory, retaining all identity/start-time/device checks in either mode.
    DIR* directory=NULL;int proc_scan=0;
    if(prefer_proc) {
        directory=opendir("/proc");proc_scan=directory!=NULL;
        if(!directory && errno!=EACCES && errno!=EPERM){int error=-errno;close(ids);return error;}
    }
    if(!directory) {
        int copy=fcntl(ids,F_DUPFD_CLOEXEC,0);if(copy<0){int error=-errno;close(ids);return error;}
        directory=fdopendir(copy);if(!directory){int error=-errno;close(copy);close(ids);return error;}
    }
    int result=0;struct dirent* entry;
    for(;;) {
        errno=0;entry=readdir(directory);if(!entry){if(errno)result=-errno;break;}
        entries++;
        pid_t pid;if(!holder_number32(entry->d_name,&pid) || pid==getpid())continue;
        uint64_t began=holder_clock32();
        char path[64];snprintf(path,sizeof(path),"/proc/%d",pid);int process=-1;
        if(proc_scan) {
            process=openat(dirfd(directory),entry->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(process<0)continue;
            struct stat owner;if(fstat(process,&owner) || owner.st_uid!=getuid()){close(process);continue;}
        }
        int record=openat(ids,entry->d_name,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
        if(record<0){if(process>=0)close(process);continue;}
        records++;
        struct stat record_stat;struct PocketHolderIdentity32 identity;
        int valid=!fstat(record,&record_stat) && S_ISREG(record_stat.st_mode) && record_stat.st_uid==getuid() && record_stat.st_size==sizeof(identity);
        ssize_t bytes=valid?read(record,&identity,sizeof(identity)):0;close(record);
        if(!valid || bytes!=sizeof(identity) || identity.magic!=0x504b4944 || identity.pid!=(uint32_t)pid || identity.uid<10000 || identity.uid>=100000 || !identity.start){if(process>=0)close(process);continue;}
        ordinary++;
        if(process<0)process=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(process<0)continue;
        struct stat st;if(fstat(process,&st) || st.st_uid!=getuid() || holder_start32(process)!=identity.start){close(process);continue;}
        live++;
        holder_slow32("identity",pid,"-",cached_stat,began);began=holder_clock32();
        int uses=holder_uses32(process,device,cached_stat,pid);holder_slow32("discovery",pid,"-",cached_stat,began);
        if(uses<=0){close(process);if(uses<0){result=uses;break;}continue;}
        int target=(int)syscall(__NR_pidfd_open,pid,0);if(target<0){int error=errno;close(process);if(error==ESRCH)continue;result=-error;break;}
        // The second lookup through a newly opened proc directory binds the
        // pidfd to the identity checked above, even if PID reuse raced open.
        int current=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(current<0 || holder_start32(current)!=identity.start){if(current>=0)close(current);close(target);close(process);continue;}
        close(current);close(process);
        if(uses) {
            if((size_t)result==capacity){close(target);result=-E2BIG;break;}
            if(syscall(__NR_pidfd_send_signal,target,SIGKILL,NULL,0) && errno!=ESRCH){result=-errno;close(target);break;}
            struct pollfd wait={target,POLLIN,0};int ready;
            began=holder_clock32();
            do{ready=poll(&wait,1,5000);}while(ready<0 && errno==EINTR);
            holder_slow32("pidfd_exit",pid,"-",cached_stat,began);
            if(ready<=0 || !(wait.revents&POLLIN)){result=ready<0?-errno:-ETIMEDOUT;close(target);break;}
            retired[result++]=pid;
        }
        close(target);
    }
    closedir(directory);close(ids);
    unsigned long long elapsed=(unsigned long long)((holder_clock32()-total_began)/1000000ULL);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO,"PocketHolders32","phase=summary scan=%s entries=%u profile_records=%u ordinary=%u live=%u result=%d cached_stat=%d elapsed_ms=%llu",proc_scan?"proc":"identity",entries,records,ordinary,live,result,cached_stat,elapsed);
#else
    fprintf(stderr,"HOLDER_SUMMARY32 scan=%s entries=%u profile_records=%u ordinary=%u live=%u result=%d cached_stat=%d elapsed_ms=%llu\n",proc_scan?"proc":"identity",entries,records,ordinary,live,result,cached_stat,elapsed);
#endif
    return result;
#endif
}
static int pocket_retire_holders32(const char* root,dev_t device,pid_t* retired,size_t capacity,int cached_stat) {
    return pocket_retire_holders_scan32(root,device,retired,capacity,cached_stat,1);
}
