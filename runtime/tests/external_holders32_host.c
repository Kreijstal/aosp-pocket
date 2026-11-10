// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/inotify.h>
#include "../storage/native/external_holders32.h"

struct Child {pid_t pid;dev_t device;};
static struct Child child(int kind) {
    int ready[2];assert(!pipe(ready));pid_t pid=fork();assert(pid>=0);
    if(!pid) {
        close(ready[0]);dev_t device=0;
        if(kind) {
            int fd=memfd_create("owned-volume-holder",MFD_CLOEXEC);assert(fd>=0 && !ftruncate(fd,4096));
            struct stat st;assert(!fstat(fd,&st));device=st.st_dev;
            if(kind==2){void* mapping=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);assert(mapping!=MAP_FAILED);memcpy(mapping,"mapped-canary",14);assert(!close(fd));}
        }
        assert(write(ready[1],&device,sizeof(device))==sizeof(device));close(ready[1]);for(;;)pause();
    }
    close(ready[1]);dev_t device;assert(read(ready[0],&device,sizeof(device))==sizeof(device));close(ready[0]);return (struct Child){pid,device};
}
static void identity(const char* root,struct Child child,uint32_t uid,int stale) {
    char path[PATH_MAX];snprintf(path,sizeof(path),"/proc/%d",child.pid);int process=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(process>=0);
    uint64_t start=holder_start32(process);assert(start);close(process);
    struct PocketHolderIdentity32 value={0x504b4944,(uint32_t)child.pid,uid,uid,start+(stale?1:0)};
    snprintf(path,sizeof(path),"%s/.pocket-identity/%d",root,child.pid);int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);assert(fd>=0);
    assert(write(fd,&value,sizeof(value))==sizeof(value) && !close(fd));
}
static void clear(const char* root,struct Child child) {
    char path[PATH_MAX];snprintf(path,sizeof(path),"%s/.pocket-identity/%d",root,child.pid);assert(!unlink(path));
}
static void trial(int cached_stat,int prefer_proc) {
    char root[]="/tmp/pocket-holder32-XXXXXX",other[]="/tmp/pocket-holder32-foreign-XXXXXX",path[PATH_MAX];
    assert(mkdtemp(root) && mkdtemp(other));snprintf(path,sizeof(path),"%s/.pocket-identity",root);assert(!mkdir(path,0700));
    snprintf(path,sizeof(path),"%s/.pocket-identity",other);assert(!mkdir(path,0700));
    struct Child children[]={child(1),child(2),child(0),child(1),child(1),child(1)};
    dev_t device=children[0].device;assert(device && children[1].device==device);
    for(unsigned i=0;i<6;i++)identity(i==5?other:root,children[i],i==4?1000:10001+i,i==3);
    FILE* limit=fopen("/proc/sys/kernel/pid_max","r");int max_pid=0;assert(limit && fscanf(limit,"%d",&max_pid)==1 && !fclose(limit) && max_pid<INT_MAX-256);
    for(unsigned i=0;i<128;i++) {
        struct PocketHolderIdentity32 stale={0x504b4944,(uint32_t)(max_pid+1+(int)i),10001,10001,1};
        snprintf(path,sizeof(path),"%s/.pocket-identity/%u",root,stale.pid);int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);assert(fd>=0 && write(fd,&stale,sizeof(stale))==sizeof(stale) && !close(fd));
    }
    snprintf(path,sizeof(path),"%s/.pocket-identity/%d",root,max_pid+129);assert(!mkfifo(path,0600));
    int watch=inotify_init1(IN_NONBLOCK|IN_CLOEXEC);snprintf(path,sizeof(path),"%s/.pocket-identity",root);assert(watch>=0 && inotify_add_watch(watch,path,IN_OPEN)>=0);
    pid_t retired[16]={0};int result=prefer_proc?pocket_retire_holders32(root,device,retired,16,cached_stat):pocket_retire_holders_scan32(root,device,retired,16,cached_stat,0);assert(result==2);
    assert((retired[0]==children[0].pid && retired[1]==children[1].pid) || (retired[1]==children[0].pid && retired[0]==children[1].pid));
    for(unsigned i=0;i<2;i++){int status;assert(waitpid(children[i].pid,&status,0)==children[i].pid && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);}
    assert((prefer_proc?pocket_retire_holders32(root,device,retired,16,cached_stat):pocket_retire_holders_scan32(root,device,retired,16,cached_stat,0))==0);
    unsigned stale_opens=0;char events[16384];ssize_t count;
    while((count=read(watch,events,sizeof(events)))>0)for(size_t at=0;at<(size_t)count;) {
        struct inotify_event event;assert((size_t)count-at>=sizeof(event));memcpy(&event,events+at,sizeof(event));assert(event.len<=(size_t)count-at-sizeof(event));
        if(event.len){pid_t pid;if(holder_number32(events+at+sizeof(event),&pid) && pid>max_pid)stale_opens++;}
        at+=sizeof(event)+event.len;
    }
    assert(count<0 && errno==EAGAIN && !close(watch));assert(prefer_proc?stale_opens==0:stale_opens>0);
    for(unsigned i=2;i<6;i++){assert(!kill(children[i].pid,0));assert(!kill(children[i].pid,SIGKILL));int status;assert(waitpid(children[i].pid,&status,0)==children[i].pid);}
    for(unsigned i=0;i<6;i++)clear(i==5?other:root,children[i]);
    for(unsigned i=0;i<129;i++){snprintf(path,sizeof(path),"%s/.pocket-identity/%d",root,max_pid+1+(int)i);assert(!unlink(path));}
    snprintf(path,sizeof(path),"%s/.pocket-identity",root);assert(!rmdir(path) && !rmdir(root));
    snprintf(path,sizeof(path),"%s/.pocket-identity",other);assert(!rmdir(path) && !rmdir(other));
}
int main(void) {
    trial(0,0);trial(1,0);trial(0,1);trial(1,1);
    puts("EXTERNAL_HOLDERS32_HOST_PASS real_pidfd_fd_and_mmap_holders=2 nonholder_preserved=1 stale_start_denied=1 system_role_preserved=1 foreign_profile_preserved=1 idempotent=1 cached_statx_and_legacy=1 proc_and_identity_discovery=1 stale_records_not_opened=1 malformed_fifo_nonblocking=1");return 0;
}
