// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/mman.h>
#include "../loader/binder_identity.h"
#define PATH_LIMIT 4096
static struct {char root[1024];} pocket_loader_config;
static size_t root_size;
static unsigned external_storage_mode,guest_uid[3];
static int data_alias=1;
static long raw(long nr,long a,long b,long c,long d,long e,long f) {long n=syscall(nr,a,b,c,d,e,f);return n<0?-errno:n;}
static size_t length(const char* text,size_t size) {return strnlen(text,size);}
static int equal(const char* a,const char* b,size_t size) {return !memcmp(a,b,size);}
static int prefix(const char* text,const char* head,size_t size) {return !strncmp(text,head,size) && (!text[size] || text[size]=='/');}
static size_t proc_number(char* out,unsigned value) {return (size_t)sprintf(out,"%u",value);}
static void proc_fd_path(int fd,char out[64]) {snprintf(out,64,"/proc/self/fd/%d",fd);}
// Match the loader's checked pipe copy, which returns the byte count.
static long copy_to_user(uintptr_t output,const char* value,size_t size) {
    int pipes[2];long rc=raw(__NR_pipe2,(long)pipes,O_CLOEXEC,0,0,0,0);
    if(rc<0)return rc;
    rc=raw(__NR_write,pipes[1],(long)value,size,0,0,0);
    if(rc==(long)size)rc=raw(__NR_read,pipes[0],output,size,0,0,0);
    else if(rc>=0)rc=-EIO;
    close(pipes[0]);close(pipes[1]);return rc;
}
#include "../loader/external_runtime.h"
static unsigned descriptors(void) {
    DIR* dir=opendir("/proc/self/fd");assert(dir);unsigned n=0;struct dirent* entry;
    while((entry=readdir(dir)))if(entry->d_name[0]!='.')n++;
    closedir(dir);return n;
}
static int listener,fixture;
static void* serve(void* ignored) {
    (void)ignored;
    for(unsigned round=0;round<9;round++) {
        int client=accept4(listener,NULL,NULL,SOCK_CLOEXEC);assert(client>=0);
        struct PocketExternalOpen request;assert(recv(client,&request,sizeof(request),MSG_WAITALL)==sizeof(request));
        if(round>=5) {
            assert(request.mode==1 && !request.path_size && request.reserved[0]==3);
            struct PocketExternalStatfsReply stats={0};stats.blocks=99;stats.bfree=25;stats.bavail=round==6?26:12;stats.files=43;stats.ffree=11;stats.bsize=4096;stats.namelen=255;stats.frsize=4096;
            if(round==7)stats.error=-EBADF;
            assert(send(client,&stats,1,MSG_NOSIGNAL)==1);assert(send(client,(char*)&stats+1,sizeof(stats)-1,MSG_NOSIGNAL)==sizeof(stats)-1);close(client);continue;
        }
        char path[128]={0};assert(request.magic==POCKET_EXTERNAL_OPEN_MAGIC && request.path_size<sizeof(path));
        assert(recv(client,path,request.path_size,MSG_WAITALL)==request.path_size && !strcmp(path,"/storage/emulated/0/file"));
        struct PocketExternalReply reply={round==4?-EACCES:0,0};
        unsigned count=round==2?2:round==3?9:round==4?0:1;
        char control[CMSG_SPACE(9*sizeof(int))]={0};struct iovec part={&reply,1};struct msghdr message={0};message.msg_iov=&part;message.msg_iovlen=1;
        if(count) {
            message.msg_control=control;message.msg_controllen=CMSG_SPACE(count*sizeof(int));struct cmsghdr* c=CMSG_FIRSTHDR(&message);
            c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(count*sizeof(int));
            for(unsigned i=0;i<count;i++)memcpy((char*)CMSG_DATA(c)+i*sizeof(int),&fixture,sizeof(fixture));
        }
        assert(sendmsg(client,&message,MSG_NOSIGNAL)==1);assert(send(client,(char*)&reply+1,sizeof(reply)-1,MSG_NOSIGNAL)==sizeof(reply)-1);close(client);
    }
    return NULL;
}
int main(void) {
    unsigned initial=descriptors();char directory[]="/tmp/pocket-external-loader-XXXXXX";assert(mkdtemp(directory));
    strcpy(pocket_loader_config.root,directory);root_size=strlen(directory);guest_uid[1]=10001;
    char path[PATH_LIMIT],out[PATH_LIMIT],temporary[PATH_LIMIT],scratch[4*PATH_LIMIT];
    external_files_start();assert(!external_files_enabled);
    external_root_path(path,"/.pocket-external-enabled");int fd=open(path,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);assert(fd>=0);close(fd);external_files_start();assert(external_files_enabled);assert(unlink(path)==0);
    assert(external_file_path(AT_FDCWD,"/sdcard/Android/media/file","/unused",out,temporary)==1 && !strcmp(out,"/storage/emulated/0/Android/media/file"));
    assert(external_file_path(AT_FDCWD,"/storage/self/primary/file","/unused",out,temporary)==1 && !strcmp(out,"/storage/emulated/0/file"));
    external_storage_mode=3;assert(external_file_path(AT_FDCWD,"/storage/emulated/0/file","/unused",out,temporary)==0);
    assert(external_file_path(AT_FDCWD,"/mnt/user/0/emulated/0/file","/unused",out,temporary)==1);external_storage_mode=0;
    for(const char** name=(const char*[]){"/system-data","/system-data/media","/system-data/media/0",NULL};*name;name++){external_root_path(path,*name);assert(mkdir(path,0700)==0);}
    external_root_path(path,"/system-data/media/0/file");fd=open(path,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);assert(fd>=0 && write(fd,"data",4)==4);close(fd);fixture=open(path,O_RDONLY|O_CLOEXEC);assert(fixture>=0);
    assert(external_file_path(AT_FDCWD,"/data/media/0/file",path,out,temporary)==1 && !strcmp(out,"/storage/emulated/0/file"));
    external_root_path(path,"/system-data/media/0");int dir=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(dir>=0);
    assert(external_file_path(dir,"file","file",out,temporary)==1 && !strcmp(out,"/storage/emulated/0/file"));close(dir);
    assert(external_open_fd("/storage/emulated/0/file",O_RDONLY,scratch)==-ENOENT);
    char stat[4096];fd=open("/proc/self/stat",O_RDONLY);assert(fd>=0);ssize_t n=read(fd,stat,sizeof(stat));close(fd);assert(n>0);
    struct PocketExternalLocator info={POCKET_EXTERNAL_LOCATOR_MAGIC,1,(uint32_t)getpid(),0,pocket_process_start(stat,n)};assert(info.start);
    external_root_path(path,"/dev");assert(mkdir(path,0700)==0);external_root_path(path,"/dev/socket");assert(mkdir(path,0700)==0);
    external_root_path(path,"/dev/socket/pocket_efs");listener=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);assert(listener>=0);
    struct sockaddr_un address={0};address.sun_family=AF_UNIX;assert(strlen(path)<sizeof(address.sun_path));strcpy(address.sun_path,path);
    assert(bind(listener,(struct sockaddr*)&address,sizeof(address))==0 && listen(listener,8)==0);
    external_root_path(path,"/.pocket-external-ready");fd=open(path,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);assert(fd>=0 && write(fd,&info,sizeof(info))==sizeof(info));close(fd);
    pthread_t thread;assert(pthread_create(&thread,NULL,serve,NULL)==0);
    for(unsigned round=0;round<5;round++) {
        unsigned flags=O_RDONLY|(round==1?O_CLOEXEC:0);long actual=external_open_fd("/storage/emulated/0/file",flags,scratch);
        if(round<2){assert(actual>=0 && !!(fcntl(actual,F_GETFD)&FD_CLOEXEC)==!!(flags&O_CLOEXEC));char bytes[4];assert(read(actual,bytes,4)==4 && !memcmp(bytes,"data",4));assert(lseek(fixture,0,SEEK_SET)==0);close(actual);}
        else assert(actual==(round==4?-EACCES:-EPROTO));
    }
    struct statfs stats;memset(&stats,0,sizeof(stats));
    assert(external_statfs_rpc(3,(uintptr_t)&stats,scratch)==0 && stats.f_type==0x65735546 && stats.f_blocks==99 && stats.f_bfree==25 && stats.f_bavail==12 && stats.f_files==43 && stats.f_ffree==11 && stats.f_bsize==4096 && stats.f_namelen==255 && stats.f_frsize==4096);
    assert(external_statfs_rpc(3,(uintptr_t)&stats,scratch)==-EPROTO);
    assert(external_statfs_rpc(3,(uintptr_t)&stats,scratch)==-EBADF);
    assert(external_statfs_rpc(3,1,scratch)==-EFAULT);
    pthread_join(thread,NULL);close(listener);close(fixture);assert(unlink(path)==0);
    external_root_path(path,"/dev/socket/pocket_efs");assert(unlink(path)==0);
    for(const char** name=(const char*[]){"/dev/socket","/dev",NULL};*name;name++){external_root_path(path,*name);assert(rmdir(path)==0);}
    external_root_path(path,"/system-data/media/0/file");assert(unlink(path)==0);
    for(const char** name=(const char*[]){"/system-data/media/0","/system-data/media","/system-data",NULL};*name;name++){external_root_path(path,*name);assert(rmdir(path)==0);}
    assert(rmdir(directory)==0 && descriptors()==initial);
    puts("EXTERNAL_LOADER_PASS aliases=1 lower_alias=1 dirfd=1 provider_lower=1 forced_fuse=1 not_mounted=1 partial_reply_rights=1 cloexec=1 excess_rights=1 truncated_rights=1 statfs=1 bad_statfs=1 bad_statfs_buffer=1 errno=1 fd_leaks=0");
}
