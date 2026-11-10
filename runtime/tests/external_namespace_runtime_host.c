// SPDX-License-Identifier: GPL-3.0-only
// C adapter for exercising the actual freestanding client/dispatch against
// the C++ broker on the host. Kernel proxy mount semantics need Android trials.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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
static size_t root_size,page_size;
static unsigned external_storage_mode,guest_uid[3];
static int data_alias=1;
static long raw(long nr,long a,long b,long c,long d,long e,long f){
    // When this adapter itself runs through the ARM64 loader, libc syscall's
    // getuid is a guest identity. The real client uses the loader's bypass
    // instruction; reproduce that kernel UID using real socket credentials.
    if(nr==__NR_getuid){int pair[2];if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair))return -errno;struct ucred peer;socklen_t size=sizeof(peer);int rc=getsockopt(pair[0],SOL_SOCKET,SO_PEERCRED,&peer,&size);int error=errno;close(pair[0]);close(pair[1]);return rc?-error:(long)peer.uid;}
    long n=syscall(nr,a,b,c,d,e,f);return n<0?-errno:n;
}
static size_t length(const char* s,size_t n){return strnlen(s,n);}
static int equal(const char* a,const char* b,size_t n){return !memcmp(a,b,n);}
static int prefix(const char* a,const char* b,size_t n){return !strncmp(a,b,n) && (!a[n] || a[n]=='/');}
static size_t proc_number(char* out,unsigned n){return (size_t)sprintf(out,"%u",n);}
static void proc_fd_path(int fd,char out[64]){snprintf(out,64,"/proc/self/fd/%d",fd);}
static long copy_path(char out[PATH_LIMIT],uintptr_t address) {
    if(address>UINTPTR_MAX-PATH_LIMIT)return -EFAULT;
    int pipes[2];long result=raw(__NR_pipe2,(long)pipes,O_CLOEXEC,0,0,0,0);if(result<0)return result;
    size_t used=0;result=-ENAMETOOLONG;
    while(used<PATH_LIMIT) {
        size_t count=page_size-((address+used)&(page_size-1));if(count>PATH_LIMIT-used)count=PATH_LIMIT-used;
        long written=raw(__NR_write,pipes[1],address+used,count,0,0,0);if(written<=0){result=written<0?written:-EFAULT;break;}
        long got=raw(__NR_read,pipes[0],(long)(out+used),written,0,0,0);if(got!=written){result=got<0?got:-EIO;break;}
        for(long i=0;i<got;i++)if(!out[used+(size_t)i]){result=(long)used+i;goto done;}
        used+=(size_t)got;if((size_t)written<count){result=-EFAULT;break;}
    }
done:close(pipes[0]);close(pipes[1]);return result;
}
static long copy_to_user(uintptr_t output,const char* value,size_t size) {
    int pipes[2];long rc=raw(__NR_pipe2,(long)pipes,O_CLOEXEC,0,0,0,0);if(rc<0)return rc;
    rc=raw(__NR_write,pipes[1],(long)value,size,0,0,0);if(rc==(long)size)rc=raw(__NR_read,pipes[0],output,size,0,0,0);else if(rc>=0)rc=-EIO;
    close(pipes[0]);close(pipes[1]);return rc;
}
#include "../loader/external_runtime.h"
static int external_kernel_active(void){return 0;}
#include "../loader/external_namespace_runtime.h"
int external_namespace_fixture_init(const char* root,unsigned uid) {
    if(strlen(root)>=sizeof(pocket_loader_config.root))return -ENAMETOOLONG;
    strcpy(pocket_loader_config.root,root);root_size=strlen(root);page_size=(size_t)sysconf(_SC_PAGESIZE);guest_uid[1]=uid;
    external_files_start();return external_namespace_enabled;
}
long external_namespace_fixture_fd(long nr,int fd,uintptr_t value) {
    __u64 r[6]={(unsigned)fd,value,0,0,0,0};return external_statfs_dispatch(nr,r) || external_directory_fd_dispatch(nr,r) || external_namespace_fd_dispatch(nr,r)?(long)r[0]:-ENOMSG;
}
long external_namespace_fixture_directory(long nr,int fd,uintptr_t value,uint64_t third) {
    __u64 r[6]={(unsigned)fd,value,third,0,0,0};return external_directory_fd_dispatch(nr,r)?(long)r[0]:-ENOMSG;
}
long external_namespace_fixture_path(long nr,const uint64_t args[6],const char* original,const char* mapped,const char* secondMapped) {
    __u64 r[6];for(unsigned i=0;i<6;i++)r[i]=args[i];char scratch[5*PATH_LIMIT];
    return external_namespace_path_dispatch(nr,r,original,mapped,secondMapped,scratch)?(long)r[0]:-ENOMSG;
}
long external_namespace_fixture_umask(void){char scratch[PATH_LIMIT];return external_current_umask(scratch);}
void external_namespace_fixture_mode(unsigned mode){external_storage_mode=mode;}
