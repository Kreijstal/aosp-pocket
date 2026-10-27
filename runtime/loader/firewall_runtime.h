// SPDX-License-Identifier: GPL-3.0-only
// Included after the interpreter's checked-copy and raw-syscall helpers.
#include "firewall.h"
#include "network_policy.h"
#include <netinet/in.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/uio.h>

static long network_policy(unsigned uid,int loopback) {
    char path[1152];
    const char* suffix=data_alias?"/system-data/misc/pocket-network-policy.rules":POCKET_NP_PATH;
    size_t tail=length(suffix,100);
    memcpy(path,pocket_loader_config.root,root_size);memcpy(path+root_size,suffix,tail+1);
    long fd=raw(__NR_openat,AT_FDCWD,(long)path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW,0,0,0);
    if(fd==-ENOENT)return 0;
    if(fd<0)return -EACCES;
    struct stat st;struct PocketNetworkPolicyHeader header;long result=-EACCES;
    if(raw(__NR_fstat,fd,(long)&st,0,0,0,0)<0 || !S_ISREG(st.st_mode) || st.st_size!=sizeof(struct PocketNetworkPolicy))goto done;
    long count;
    do {count=raw(__NR_pread64,fd,(long)&header,sizeof(header),0,0,0);}while(count==-EINTR);
    if(count!=sizeof(header) || !pocket_np_valid(&header))goto done;
    if(!header.enabled){result=0;goto done;}
    if(uid>=POCKET_NP_UIDS)goto done;
    for(unsigned chain=0;chain<POCKET_NP_CHAINS;chain++){
        if(!(header.enabled&(1U<<chain)))continue;
        if(loopback && chain!=POCKET_NP_NO_INTERNET)continue;
        uint8_t byte=0;
        size_t offset=offsetof(struct PocketNetworkPolicy,members)+chain*POCKET_NP_BYTES+uid/8;
        do {count=raw(__NR_pread64,fd,(long)&byte,1,offset,0,0);}while(count==-EINTR);
        if(count!=1 || pocket_np_member_denies(chain,uid,!!(byte&(1U<<(uid%8)))))goto done;
    }
    result=0;
done:raw(__NR_close,fd,0,0,0,0,0);return result;
}

static long firewall_address(int fd,uintptr_t pointer,size_t size) {
    struct sockaddr_storage address;unsigned address_size=sizeof(address);
    long rc;
    // Like the kernel, a zero-length address means none: Rust's std passes
    // a non-NULL msg_name with msg_namelen 0 for connected UNIX sockets.
    if(!size)pointer=0;
    if(pointer) {
        if(size<sizeof(sa_family_t))return -EINVAL;
        memset(&address,0,sizeof(address));
        rc=copy_from_user(&address,pointer,size<sizeof(address)?size:sizeof(address));
    } else rc=raw(__NR_getpeername,fd,(long)&address,(long)&address_size,0,0,0);
    // Non-sockets, unconnected sockets and UNIX IPC keep their kernel behaviour.
    if(rc<0)return pointer?rc:0;
    if(address.ss_family!=AF_INET && address.ss_family!=AF_INET6)return 0;
    if(pointer && size<(address.ss_family==AF_INET?sizeof(struct sockaddr_in):sizeof(struct sockaddr_in6)))return -EINVAL;
    unsigned uid=guest_identity?guest_uid[1]:(unsigned)raw(__NR_geteuid,0,0,0,0,0,0);
    int loopback=0;
    if(address.ss_family==AF_INET)loopback=((const uint8_t*)&((struct sockaddr_in*)&address)->sin_addr)[0]==127;
    else {
        const uint8_t* bytes=(const uint8_t*)&((struct sockaddr_in6*)&address)->sin6_addr;
        static const uint8_t local[16]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        static const uint8_t mapped[12]={0,0,0,0,0,0,0,0,0,0,255,255};
        loopback=equal((const char*)bytes,(const char*)local,16) ||
            (equal((const char*)bytes,(const char*)mapped,12) && bytes[12]==127);
    }
    rc=network_policy(uid,loopback);if(rc<0)return rc;
    char path[1152];
    const char* suffix=data_alias?"/system-data/misc/pocket-firewall.rules":POCKET_FW_PATH;
    size_t tail=length(suffix,100);
    memcpy(path,pocket_loader_config.root,root_size);memcpy(path+root_size,suffix,tail+1);
    long rules_fd=raw(__NR_openat,AT_FDCWD,(long)path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW,0,0,0);
    if(rules_fd==-ENOENT)return 0; // No configured firewall: preserve existing behaviour.
    if(rules_fd<0)return -EACCES;
    long memory=raw(__NR_mmap,0,sizeof(struct PocketFirewall)+1,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(memory<0) {raw(__NR_close,rules_fd,0,0,0,0,0);return -EACCES;}
    size_t used=0;
    while(used<sizeof(struct PocketFirewall)+1) {
        rc=raw(__NR_read,rules_fd,memory+used,sizeof(struct PocketFirewall)+1-used,0,0,0);
        if(rc==-EINTR)continue;
        if(rc<=0)break;
        used+=(size_t)rc;
    }
    raw(__NR_close,rules_fd,0,0,0,0,0);
    struct PocketFirewall* rules=(struct PocketFirewall*)memory;
    long result=-EACCES;
    if(rc>=0 && used==sizeof(*rules) && pocket_fw_valid(rules)) {
        int protocol=0;unsigned len=sizeof(protocol);
        if(raw(__NR_getsockopt,fd,SOL_SOCKET,SO_PROTOCOL,(long)&protocol,(long)&len,0)>=0) {
            const uint8_t* bytes;unsigned port,family;
            if(address.ss_family==AF_INET) {
                struct sockaddr_in* a=(struct sockaddr_in*)&address;
                bytes=(const uint8_t*)&a->sin_addr;port=((const uint8_t*)&a->sin_port)[0]*256U+((const uint8_t*)&a->sin_port)[1];family=4;
            } else {
                struct sockaddr_in6* a=(struct sockaddr_in6*)&address;
                bytes=(const uint8_t*)&a->sin6_addr;port=((const uint8_t*)&a->sin6_port)[0]*256U+((const uint8_t*)&a->sin6_port)[1];family=6;
                // IPv4-mapped IPv6 sockets must obey the IPv4 rules too.
                static const uint8_t mapped[12]={0,0,0,0,0,0,0,0,0,0,255,255};
                if(equal((const char*)bytes,(const char*)mapped,12)) {bytes+=12;family=4;}
            }
            result=pocket_fw_decide(rules,uid,family,(unsigned)protocol,bytes,port)?-EACCES:0;
        }
    }
    raw(__NR_munmap,memory,sizeof(*rules)+1,0,0,0,0);return result;
}
static long firewall_message(int fd,uintptr_t pointer) {
    struct msghdr message;long rc=copy_from_user(&message,pointer,sizeof(message));
    if(rc<0)return rc;
    return firewall_address(fd,(uintptr_t)message.msg_name,message.msg_namelen);
}
static int firewall_dispatch(long nr,__u64* r) {
    long rc;
    if(nr==__NR_sendto)rc=firewall_address((int)r[0],r[4],r[5]);
    else if(nr==__NR_sendmsg)rc=firewall_message((int)r[0],r[1]);
    else if(nr==__NR_sendmmsg) {
        if(r[2]>1024) {r[0]=(uint64_t)-EINVAL;return 1;}
        rc=0;
        for(size_t i=0;i<r[2] && !rc;i++)rc=firewall_message((int)r[0],r[1]+i*sizeof(struct mmsghdr));
    } else if(nr==__NR_write || nr==__NR_writev || nr==__NR_sendfile || nr==__NR_copy_file_range || nr==__NR_splice) {
        int fd=(int)((nr==__NR_copy_file_range || nr==__NR_splice)?r[2]:r[0]);
        rc=firewall_address(fd,0,0);
    } else return 0;
    r[0]=rc<0?rc:raw(nr,r[0],r[1],r[2],r[3],r[4],r[5]);return 1;
}
