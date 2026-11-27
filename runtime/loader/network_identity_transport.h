// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// A procfs descriptor pins the caller even when hidepid prevents another
// process from reopening its stat pathname after PR_SET_DUMPABLE=0.
#include "network_identity_protocol.h"

// raw() is the interpreter's negative-errno syscall entry.
static long pocket_network_send_identity(int socket_fd) {
    long proof=raw(__NR_openat,AT_FDCWD,(long)"/proc/self/stat",O_RDONLY|O_CLOEXEC,0,0,0);
    if(proof<0)return proof;
    union {struct cmsghdr alignment;unsigned char bytes[CMSG_SPACE(sizeof(int))];} control;
    memset(&control,0,sizeof(control));
    struct iovec part={(void*)pocket_network_identity_header,sizeof(pocket_network_identity_header)};
    struct msghdr message;memset(&message,0,sizeof(message));
    message.msg_iov=&part;message.msg_iovlen=1;
    message.msg_control=control.bytes;message.msg_controllen=sizeof(control.bytes);
    struct cmsghdr* header=CMSG_FIRSTHDR(&message);
    header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(int));
    int descriptor=(int)proof;memcpy(CMSG_DATA(header),&descriptor,sizeof(descriptor));
    long count;
    do{count=raw(__NR_sendmsg,socket_fd,(long)&message,MSG_NOSIGNAL,0,0,0);}while(count==-EINTR);
    raw(__NR_close,proof,0,0,0,0,0);
    if(count<=0)return count<0?count:-EIO;
    size_t offset=(size_t)count;
    if(offset>sizeof(pocket_network_identity_header))return -EIO;
    while(offset<sizeof(pocket_network_identity_header)) {
        count=raw(__NR_sendto,socket_fd,(long)(pocket_network_identity_header+offset),
                  sizeof(pocket_network_identity_header)-offset,MSG_NOSIGNAL,0,0);
        if(count==-EINTR)continue;
        if(count<=0)return count<0?count:-EIO;
        offset+=(size_t)count;
    }
    return 0;
}
