// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fuse_transport.h"
#include "../loader/binder_identity.h"
#include <fcntl.h>
#include <cstdio>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <linux/magic.h>

namespace pocket::fuse32 {
// The real kernel UID remains the Manager application's UID. Derive it from
// kernel socket credentials, since getuid() inside the guest is virtualized.
inline int kernelUid(uid_t* uid) {
    int pair[2];if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair))return -errno;
    ucred self{};socklen_t size=sizeof(self);int result=getsockopt(pair[0],SOL_SOCKET,SO_PEERCRED,&self,&size)?-errno:0;
    close(pair[0]);close(pair[1]);if(result<0)return result;
    if(size!=sizeof(self) || self.pid<=0)return -EPROTO;
    *uid=self.uid;return 0;
}
inline ssize_t readIdentityFile(const char* path,void* bytes,size_t size) {
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -errno;
    ssize_t count;do{count=read(fd,bytes,size);}while(count<0 && errno==EINTR);
    int error=errno;close(fd);return count<0?-error:count;
}
// The parent opens its own stat file and sends it over an authenticated socket:
// Android can hide a non-debuggable parent's proc directory from its children.
inline bool lifetimeDescriptor(int fd,pid_t process,uint64_t start) {
    struct statfs filesystem{};
    if(process<=0 || !start || fstatfs(fd,&filesystem) || filesystem.f_type!=PROC_SUPER_MAGIC)return false;
    char path[64],expected[64],target[128],stat[4096];
    snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);snprintf(expected,sizeof(expected),"/proc/%d/stat",process);
    ssize_t n=readlink(path,target,sizeof(target)-1);
    if(n<0 || n>=static_cast<ssize_t>(sizeof(target)-1))return false;
    target[n]=0;if(strcmp(target,expected))return false;
    do{n=pread(fd,stat,sizeof(stat),0);}while(n<0 && errno==EINTR);
    return n>0 && pocket_process_start(stat,n)==start;
}
// Rootless guest metadata, not a separate kernel identity/security boundary.
// Only a tid is carried in the client request; UID/GID come from the loader's
// live PID/start-time record. A fresh connection per metadata request avoids
// trusting SO_PEERCRED inherited from a pre-fork connection.
inline int threadIdentity(pid_t process,pid_t tid,Identity* identity,const char* identityRoot="/.pocket-identity") {
    if(process<=0 || tid<=0 || !identity)return -EACCES;
    char path[96],status[4096];
    // /proc/<tgid>/task/<tid> exists only for a thread of this peer process.
    // MediaProvider's PendingOpenInfo uses this tid, not the process's tgid.
    snprintf(path,sizeof(path),"/proc/%d/task/%d/stat",process,tid);
    ssize_t count=readIdentityFile(path,status,sizeof(status));
    if(count<=0 || !pocket_process_start(status,static_cast<size_t>(count)))return -EACCES;
    snprintf(path,sizeof(path),"/proc/%d/stat",process);
    count=readIdentityFile(path,status,sizeof(status));
    if(count<=0)return -EACCES;
    uint64_t start=pocket_process_start(status,static_cast<size_t>(count));if(!start)return -EACCES;
    // The trusted embedding service chooses the profile's record directory;
    // it is never carried in an app request. A host fixture can use its own
    // temporary profile without weakening the kernel PID/start-time checks.
    char recordPath[4096];int length=snprintf(recordPath,sizeof(recordPath),"%s/%d",identityRoot,process);
    if(length<0 || length>=static_cast<int>(sizeof(recordPath)))return -EACCES;
    PocketBinderIdentity record{};count=readIdentityFile(recordPath,&record,sizeof(record));
    if(count!=sizeof(record) || record.magic!=POCKET_IDENTITY_MAGIC || record.pid!=static_cast<uint32_t>(process)
       || record.start_ticks!=start)return -EACCES;
    *identity={record.uid,record.gid,static_cast<uint32_t>(tid)};return 0;
}
inline int peerIdentity(int socket,pid_t tid,uid_t expectedHostUid,Identity* identity,const char* identityRoot="/.pocket-identity") {
    ucred peer{};socklen_t size=sizeof(peer);
    if(getsockopt(socket,SOL_SOCKET,SO_PEERCRED,&peer,&size))return -errno;
    if(size!=sizeof(peer) || peer.pid<=0 || peer.uid!=expectedHostUid)return -EACCES;
    return threadIdentity(peer.pid,tid,identity,identityRoot);
}
inline int hostPeer(int socket,pid_t expectedParent,uid_t expectedHostUid) {
    ucred peer{};socklen_t size=sizeof(peer);
    if(getsockopt(socket,SOL_SOCKET,SO_PEERCRED,&peer,&size))return -errno;
    return size==sizeof(peer) && expectedParent>0 && peer.pid==expectedParent && peer.uid==expectedHostUid?0:-EACCES;
}
// Only the known host parent's kernel AppFuse adapter can forward another
// process's tid. Ordinary app metadata requests must use peerIdentity instead.
inline int forwardedIdentity(int socket,pid_t tid,pid_t expectedParent,uid_t expectedHostUid,Identity* identity) {
    int rc=hostPeer(socket,expectedParent,expectedHostUid);if(rc<0 || tid<=0)return rc<0?rc:-EACCES;
    char path[64],status[4097];snprintf(path,sizeof(path),"/proc/%d/status",tid);
    ssize_t count=readIdentityFile(path,status,sizeof(status)-1);if(count<=0)return -EACCES;status[count]=0;
    pid_t group=0;unsigned uid=UINT32_MAX;
    for(const char* p=status;*p;) {
        if(!strncmp(p,"Tgid:",5)){int value=0;if(sscanf(p+5,"%d",&value)==1)group=value;}
        if(!strncmp(p,"Uid:",4))sscanf(p+4,"%u",&uid);
        const char* next=strchr(p,'\n');if(!next)break;p=next+1;
    }
    if(uid!=expectedHostUid)return -EACCES;
    return threadIdentity(group,tid,identity);
}
}
