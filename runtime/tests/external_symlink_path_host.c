// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>
#include "../loader/binder_identity.h"
#define PATH_LIMIT 4096
static struct {char root[1024];} pocket_loader_config;
static size_t root_size;
static unsigned external_storage_mode,guest_uid[3];
static unsigned denylist_view,module_view;
static int data_alias=1;
static long raw(long nr,long a,long b,long c,long d,long e,long f){long n=syscall(nr,a,b,c,d,e,f);return n<0?-errno:n;}
static size_t length(const char* p,size_t n){return strnlen(p,n);}
static int equal(const char* a,const char* b,size_t n){return !memcmp(a,b,n);}
static int prefix(const char* a,const char* b,size_t n){return !strncmp(a,b,n) && (!a[n] || a[n]=='/');}
static size_t proc_number(char* out,unsigned n){return (size_t)sprintf(out,"%u",n);}
static void proc_fd_path(int fd,char out[64]){snprintf(out,64,"/proc/self/fd/%d",fd);}
static long copy_to_user(uintptr_t out,const char* in,size_t size){memcpy((void*)out,in,size);return size;}
static int denylist_hidden(const char* path){(void)path;return 0;}
#include "../loader/path_map.h"
#include "../loader/external_runtime.h"
static void path(char* out,size_t size,const char* suffix){assert(snprintf(out,size,"%s%s",pocket_loader_config.root,suffix)>0 && strlen(out)<size-1);}
int main(void) {
    char root[]="/tmp/pocket-symlink-path-XXXXXX";assert(mkdtemp(root));
    strcpy(pocket_loader_config.root,root);root_size=strlen(root);guest_uid[1]=10001;
    char directory[4096],target[4096],link[4096],out[4096],scratch[4096],mapped[4096],relative[4096],jump[4096];
    for(const char** part=(const char*[]){"/system-data","/system-data/media","/system-data/media/0","/system-data/media/0/Android","/system-data/media/0/Android/data","/system-data/media/0/Android/data/peer","/system-data/media/0/Android/data/peer/files","/internal",0};*part;part++){
        path(directory,sizeof(directory),*part);assert(!mkdir(directory,0700));
    }
    path(target,sizeof(target),"/system-data/media/0/Android/data/peer/files/canary");
    int fd=open(target,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);assert(fd>=0);assert(!close(fd));
    path(link,sizeof(link),"/internal/peer");
    assert(!symlink(target,link));
    path(relative,sizeof(relative),"/internal/relative");
    assert(!symlink("../system-data/media/0/Android/data/peer/files/canary",relative));
    path(jump,sizeof(jump),"/internal/jump");
    path(directory,sizeof(directory),"/system-data/media/0/Android/data/peer/files");assert(!symlink(directory,jump));
    // This is the actual path-routing routine used before a following open.
    // All test paths belong to this process; no user or guest data is read.
    int direct=external_file_path(AT_FDCWD,target,target,out,scratch);
    int existing=external_file_path(AT_FDCWD,link,link,out,scratch);
    assert(!strcmp(out,"/storage/emulated/0/Android/data/peer/files/canary"));
    assert(external_file_path_nofollow(AT_FDCWD,link,link,out,scratch)==0);
    assert(external_file_path(AT_FDCWD,relative,relative,out,scratch)==1);
    unsigned pending=0;const char* guest="/internal/jump/../files/canary";
    assert(map_path(guest,mapped)==-EPERM);
    assert(!map_path_parent_probe(guest,mapped,1,&pending) && pending==1);
    assert(external_file_path(AT_FDCWD,guest,mapped,out,scratch)==1);
    assert(!strcmp(out,"/storage/emulated/0/Android/data/peer/files/canary"));
    pending=0;assert(map_path_parent_probe(guest,mapped,0,&pending)==-EPERM && !pending);
    pending=0;guest="/internal/../internal/unrelated";
    assert(!map_path_parent_probe(guest,mapped,1,&pending) && pending==1);
    // An unhandled deferred probe stays marked; path_dispatch rejects it
    // before its raw-syscall fallback. This path must not be relabeled routed.
    assert(external_file_path(AT_FDCWD,guest,mapped,out,scratch)==0 && pending==1);
    assert(!unlink(target));
    int dangling=external_file_path(AT_FDCWD,link,link,out,scratch);
    assert(!strcmp(out,"/storage/emulated/0/Android/data/peer/files/canary"));
    assert(external_file_path_nofollow(AT_FDCWD,link,link,out,scratch)==0);
    assert(external_file_path(AT_FDCWD,relative,relative,out,scratch)==1);
    assert(!strcmp(out,"/storage/emulated/0/Android/data/peer/files/canary"));
    assert(!unlink(relative) && !unlink(jump));
    assert(!unlink(link));
    for(const char** part=(const char*[]){"/internal","/system-data/media/0/Android/data/peer/files","/system-data/media/0/Android/data/peer","/system-data/media/0/Android/data","/system-data/media/0/Android","/system-data/media/0","/system-data/media","/system-data",0};*part;part++){
        path(directory,sizeof(directory),*part);assert(!rmdir(directory));
    }
    assert(!rmdir(root));
    printf("EXTERNAL_SYMLINK_PATH direct=%d existing_final_link=%d dangling_final_link=%d relative_link=1 nofollow_preserved=1 parent_probe_routed=1 unhandled_parent_stays_deferred=1 disabled_parent_probe_denied=1 cleanup=1\n",direct,existing,dangling);
    return direct==1 && existing==1 && dangling==1?0:1;
}
