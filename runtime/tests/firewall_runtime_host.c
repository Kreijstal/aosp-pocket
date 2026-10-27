// SPDX-License-Identifier: GPL-3.0-only
// Harness for the actual interpreter policy code, with checked test pointers.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/types.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>
static struct {char root[1024];} pocket_loader_config;
static size_t root_size;
static int data_alias,guest_identity=1;
static unsigned guest_uid[3];
static long raw(long n,long a,long b,long c,long d,long e,long f) {
    long rc=syscall(n,a,b,c,d,e,f);return rc==-1?-errno:rc;
}
static long copy_from_user(void* out,uintptr_t from,size_t size) {memcpy(out,(void*)from,size);return 0;}
static size_t length(const char* p,size_t n) {size_t i=0;while(i<n && p[i])i++;return i;}
static int equal(const char* a,const char* b,size_t n) {return !memcmp(a,b,n);}
#include "../loader/firewall_runtime.h"
void fw_setup(const char* root,unsigned uid) {
    root_size=strlen(root);if(root_size>=sizeof(pocket_loader_config.root))_exit(2);
    strcpy(pocket_loader_config.root,root);guest_uid[1]=uid;
}
void fw_data_alias(int enabled) {data_alias=enabled;}
long fw_call(long nr,long a,long b,long c,long d,long e,long f) {
    __u64 regs[6]={(uint64_t)a,(uint64_t)b,(uint64_t)c,(uint64_t)d,(uint64_t)e,(uint64_t)f};
    if(nr==__NR_connect) {
        long rc=firewall_address((int)a,(uintptr_t)b,(size_t)c);
        return rc<0?rc:raw(nr,a,b,c,d,e,f);
    }
    if(!firewall_dispatch(nr,regs))return -ENOSYS;
    return (long)regs[0];
}
// Do not assume host syscall numbers in Python.
long fw_nr(int which) {
    const long numbers[]={__NR_connect,__NR_sendto,__NR_write,__NR_writev,__NR_sendmsg,__NR_sendmmsg,__NR_sendfile,__NR_splice};
    return which>=0 && which<8?numbers[which]:-1;
}
