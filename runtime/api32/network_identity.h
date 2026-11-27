// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "../loader/binder_identity.h"
#include "../loader/network_identity_protocol.h"

static inline bool pocket_network_read_record(pid_t pid,const char* directory,PocketBinderIdentity* record) {
    if(pid<=0 || !directory || !record)return false;
    char path[4096];int length=snprintf(path,sizeof(path),"%s/%d",directory,pid);
    if(length<=0 || static_cast<size_t>(length)>=sizeof(path))return false;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return false;
    struct stat info{};ssize_t count;
    do{count=pread(fd,record,sizeof(*record),0);}while(count<0 && errno==EINTR);
    bool valid=!fstat(fd,&info) && S_ISREG(info.st_mode) && info.st_size==sizeof(*record)
        && count==sizeof(*record) && record->magic==POCKET_IDENTITY_MAGIC
        && record->pid==static_cast<uint32_t>(pid) && record->uid<=INT32_MAX && record->start_ticks;
    close(fd);return valid;
}

static inline bool pocket_network_stat_matches(int fd,pid_t pid,uint64_t start) {
    // A normal file or memfd containing a copied stat line is not a proof.
    struct statfs filesystem{};struct stat info{};
    if(fstatfs(fd,&filesystem) || filesystem.f_type!=0x9fa0 || fstat(fd,&info) || !S_ISREG(info.st_mode))return false;
    char link[64],expected[64],target[256];
    snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
    snprintf(expected,sizeof(expected),"/proc/%d/stat",pid);
    ssize_t length=readlink(link,target,sizeof(target)-1);
    if(length<0 || static_cast<size_t>(length)>=sizeof(target)-1)return false;
    target[length]=0;if(strcmp(target,expected))return false;
    char text[4096];ssize_t count;
    do{count=pread(fd,text,sizeof(text),0);}while(count<0 && errno==EINTR);
    return count>0 && pocket_process_start(text,static_cast<size_t>(count))==start;
}

// Kernel credentials, a real procfs FD and the existing PID/start-time record
// establish guest-role metadata. No UID is supplied by the socket payload.
// Legacy callers retain their existing fresh pathname validation.
static inline bool pocket_network_receive_identity(int socket_fd,uid_t kernel_uid,uint64_t* identity,
                                        const char* directory="/.pocket-identity") {
    if(!identity)return false;
    ucred peer{};socklen_t peer_size=sizeof(peer);
    if(getsockopt(socket_fd,SOL_SOCKET,SO_PEERCRED,&peer,&peer_size) || peer_size!=sizeof(peer)
            || peer.uid!=kernel_uid || peer.pid<=0)return false;
    PocketBinderIdentity record{};
    if(!pocket_network_read_record(peer.pid,directory,&record))return false;
    unsigned char first=0;
    // Peek without receiving ancillary data. A legacy fwmark command already
    // carries its own socket FD, so leave its bytes and SCM_RIGHTS queued.
    ssize_t peek;
    do{peek=recv(socket_fd,&first,1,MSG_PEEK);}while(peek<0 && errno==EINTR);
    if(peek!=1)return false;
    if(first!=pocket_network_identity_header[0]) {
        char path[64];snprintf(path,sizeof(path),"/proc/%d/stat",peer.pid);
        int proof=open(path,O_RDONLY|O_CLOEXEC);
        bool valid=proof>=0 && pocket_network_stat_matches(proof,peer.pid,record.start_ticks);
        if(proof>=0)close(proof);
        if(!valid)return false;
        *identity=(static_cast<uint64_t>(record.uid)<<32)|record.pid;return true;
    }
    union {struct cmsghdr alignment;unsigned char bytes[CMSG_SPACE(sizeof(int)*4)];} control{};
    iovec part{&first,1};msghdr message{};message.msg_iov=&part;message.msg_iovlen=1;
    message.msg_control=control.bytes;message.msg_controllen=sizeof(control.bytes);
    ssize_t count;
    do{count=recvmsg(socket_fd,&message,MSG_CMSG_CLOEXEC);}while(count<0 && errno==EINTR);
    int proofs[4]={-1,-1,-1,-1};size_t proof_count=0;
    bool valid=count==1 && !(message.msg_flags&(MSG_CTRUNC|MSG_TRUNC));
    if(count>=0)for(cmsghdr* item=CMSG_FIRSTHDR(&message);item;item=CMSG_NXTHDR(&message,item)) {
        if(item->cmsg_level!=SOL_SOCKET || item->cmsg_type!=SCM_RIGHTS || item->cmsg_len<CMSG_LEN(0)) {valid=false;continue;}
        size_t bytes=item->cmsg_len-CMSG_LEN(0);if(bytes%sizeof(int))valid=false;
        for(size_t i=0;i<bytes/sizeof(int);++i) {
            int proof;memcpy(&proof,CMSG_DATA(item)+i*sizeof(int),sizeof(proof));
            if(proof_count<4)proofs[proof_count++]=proof;
            else{close(proof);valid=false;}
        }
    }
    if(valid && first==pocket_network_identity_header[0]) {
        unsigned char header[8];header[0]=first;size_t used=1;
        valid=proof_count==1;
        while(valid && used<sizeof(header)) {
            do{count=recv(socket_fd,header+used,sizeof(header)-used,0);}while(count<0 && errno==EINTR);
            if(count<=0){valid=false;break;}used+=static_cast<size_t>(count);
        }
        valid=valid && !memcmp(header,pocket_network_identity_header,sizeof(header))
            && pocket_network_stat_matches(proofs[0],peer.pid,record.start_ticks);
    }else valid=false;
    for(size_t i=0;i<proof_count;++i)close(proofs[i]);
    if(!valid)return false;
    *identity=(static_cast<uint64_t>(record.uid)<<32)|record.pid;return true;
}
