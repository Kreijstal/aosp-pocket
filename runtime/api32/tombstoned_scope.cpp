// SPDX-License-Identifier: GPL-3.0-only
// Keep the stock protocol/daemon, scoped to processes in this guest profile.
#include "../api28/binder_identity.h"
#include "../direct/kernel_identity.h"
#include "protocol.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace {
enum Endpoint {Other,Intercept,Crash,Java};
Endpoint endpoint(int fd) {
    sockaddr_un address={};socklen_t length=sizeof(address);
    if(getsockname(fd,reinterpret_cast<sockaddr*>(&address),&length) || address.sun_family!=AF_UNIX)return Other;
    address.sun_path[sizeof(address.sun_path)-1]=0;
    const char* path=address.sun_path;
    auto ends=[&](const char* tail){size_t n=strlen(path),m=strlen(tail);return n>=m && !strcmp(path+n-m,tail);};
    if(ends("/dev/socket/tombstoned_intercept"))return Intercept;
    if(ends("/dev/socket/tombstoned_crash"))return Crash;
    if(ends("/dev/socket/tombstoned_java_trace"))return Java;
    return Other;
}
bool authorized(int fd,Endpoint kind,const void* bytes,size_t size) {
    static const uid_t host=pocket_kernel_uid_from_proc();
    ucred peer={};socklen_t length=sizeof(peer);uint64_t identity=0,target=0;
    if(host==static_cast<uid_t>(-1) || getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&length) ||
            length!=sizeof(peer) || peer.uid!=host)return false;
    // The authenticated crash writer may exit after sending its completion.
    // Stock tombstoned accepts this packet only in the same connection's
    // crash_completed state, after we have authorized its initial request.
    if((kind==Crash || kind==Java) && size==sizeof(TombstonedCrashPacket) &&
            static_cast<const TombstonedCrashPacket*>(bytes)->packet_type==CrashPacketType::kCompletedDump)return true;
    if(!pocket_binder_identity(peer.pid,&identity))return false;
    const uid_t uid=identity>>32;const bool privileged=uid==0 || uid==1000 || uid==2000;
    pid_t targetPid=peer.pid;
    if(kind==Intercept) {
        if(!privileged || size!=sizeof(InterceptRequest))return false;
        targetPid=static_cast<const InterceptRequest*>(bytes)->pid;
    }else if(size==sizeof(TombstonedCrashPacket)) {
        const auto* packet=static_cast<const TombstonedCrashPacket*>(bytes);
        if(packet->packet_type==CrashPacketType::kDumpRequest &&
                packet->packet.dump_request.dump_type!=kDebuggerdJavaBacktrace)
            targetPid=packet->packet.dump_request.pid;
    }
    if(!pocket_binder_identity(targetPid,&target))return false;
    return privileged || (target>>32)==uid;
}
void closeRights(msghdr* message) {
    for(cmsghdr* c=CMSG_FIRSTHDR(message);c;c=CMSG_NXTHDR(message,c)) {
        if(c->cmsg_level!=SOL_SOCKET || c->cmsg_type!=SCM_RIGHTS || c->cmsg_len<CMSG_LEN(0))continue;
        size_t count=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);
        auto* fds=reinterpret_cast<int*>(CMSG_DATA(c));for(size_t i=0;i<count;++i)close(fds[i]);
    }
    message->msg_controllen=0;
}

bool artifactName(const char* path,const char* prefix,bool proto) {
    size_t n=strlen(prefix);if(strncmp(path,prefix,n))return false;
    const char* digits=path+n;const char* end=digits;
    while(*end>='0' && *end<='9')++end;
    return end!=digits && (!*end || (proto && !strcmp(end,".pb")));
}

// Android app SELinux policy can deny linkat even for our own O_TMPFILE.
// Retain stock tombstoned's anonymous output/FD protocol and publish actual
// completed bytes via a same-directory temporary file and atomic rename.
int publishArtifact(int source,int directory,const char* path) {
    struct stat dir={},expected={},input={};
    if(fstat(directory,&dir) || !S_ISDIR(dir.st_mode) || fstat(source,&input) ||
            !S_ISREG(input.st_mode) || input.st_size<=0 || input.st_size>64*1024*1024){errno=EINVAL;return -1;}
    const char* parent=nullptr;
    const char* candidates[]={"/data/anr","/data/tombstones"};
    for(const char* candidate:candidates) {
        if(!fstatat(AT_FDCWD,candidate,&expected,AT_SYMLINK_NOFOLLOW) && S_ISDIR(expected.st_mode) &&
                dir.st_dev==expected.st_dev && dir.st_ino==expected.st_ino){parent=candidate;break;}
    }
    if(!parent || !artifactName(path,!strcmp(parent,"/data/anr")?"trace_":"tombstone_",
            !strcmp(parent,"/data/tombstones"))){errno=EINVAL;return -1;}
    char proc[64],location[PATH_MAX];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",source);
    ssize_t n=readlink(proc,location,sizeof(location)-1);
    if(n<0)return -1;
    location[n]=0;size_t prefix=strlen(parent);
    if(strncmp(location,parent,prefix) || location[prefix]!='/' || !location[prefix+1] ||
            strchr(location+prefix+1,'/')){errno=EXDEV;return -1;}
    int reader=open(proc,O_RDONLY|O_CLOEXEC);if(reader<0)return -1;
    struct stat reopened={};
    if(fstat(reader,&reopened) || reopened.st_dev!=input.st_dev || reopened.st_ino!=input.st_ino){
        close(reader);errno=ESTALE;return -1;
    }
    static unsigned counter=0;char temporary[96];int output=-1;
    for(unsigned attempt=0;attempt<8;++attempt) {
        snprintf(temporary,sizeof(temporary),".pocket-save-%d-%u",getpid(),counter++);
        output=openat(directory,temporary,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0660);
        if(output>=0 || errno!=EEXIST)break;
    }
    if(output<0){int error=errno;close(reader);errno=error;return -1;}
    int error=0;off_t offset=0;char buffer[8192];
    while(offset<input.st_size) {
        size_t wanted=sizeof(buffer);if(input.st_size-offset<static_cast<off_t>(wanted))wanted=input.st_size-offset;
        ssize_t count;do{count=pread(reader,buffer,wanted,offset);}while(count<0 && errno==EINTR);
        if(count<=0){error=count<0?errno:EIO;break;}
        ssize_t written=0;
        while(written<count) {
            ssize_t step;do{step=write(output,buffer+written,count-written);}while(step<0 && errno==EINTR);
            if(step<=0){error=step<0?errno:EIO;break;}written+=step;
        }
        if(error)break;
        offset+=count;
    }
    if(!error && (fstat(reader,&reopened) || reopened.st_size!=input.st_size))error=ESTALE;
    if(!error && (fchmod(output,0660) || fsync(output)))error=errno;
    if(close(output) && !error)error=errno;
    close(reader);
    if(!error && renameat(directory,temporary,directory,path))error=errno;
    if(error){unlinkat(directory,temporary,0);errno=error;return -1;}
    fprintf(stderr,"API32_TOMBSTONED_ARTIFACT_PUBLISHED path=%s/%s bytes=%lld atomic_rename=1\n",
            parent,path,static_cast<long long>(input.st_size));
    return 0;
}
}
extern "C" int linkat(int olddir,const char* oldpath,int newdir,const char* newpath,int flags) {
    int result=syscall(__NR_linkat,olddir,oldpath,newdir,newpath,flags);
    if(!result || (errno!=EACCES && errno!=EPERM))return result;
    const int denied=errno;const char* prefix="/proc/self/fd/";
    if(olddir!=AT_FDCWD || flags!=AT_SYMLINK_FOLLOW || strncmp(oldpath,prefix,strlen(prefix))){errno=denied;return -1;}
    const char* number=oldpath+strlen(prefix);char* end=nullptr;long fd=strtol(number,&end,10);
    if(*number<'0' || *number>'9' || *end || fd<0 || fd>INT_MAX){errno=denied;return -1;}
    return publishArtifact(static_cast<int>(fd),newdir,newpath);
}
extern "C" ssize_t read(int fd,void* data,size_t size) {
    auto result=syscall(__NR_read,fd,data,size);
    if(result<=0)return result;
    Endpoint kind=endpoint(fd);
    if(kind!=Other && !authorized(fd,kind,data,result)) {
        fprintf(stderr,"API32_TOMBSTONED_REQUEST_DENIED endpoint=%d\n",kind);errno=EACCES;return -1;
    }
    return result;
}
extern "C" ssize_t recvmsg(int fd,msghdr* message,int flags) {
    auto result=syscall(__NR_recvmsg,fd,message,flags);
    if(result<=0)return result;
    Endpoint kind=endpoint(fd);
    if(kind==Other)return result;
    alignas(TombstonedCrashPacket) unsigned char packet[sizeof(TombstonedCrashPacket)+sizeof(InterceptRequest)]={};size_t copied=0;
    for(size_t i=0;i<message->msg_iovlen && copied<sizeof(packet);++i) {
        size_t n=message->msg_iov[i].iov_len;
        if(n>static_cast<size_t>(result)-copied)n=result-copied;
        if(n>sizeof(packet)-copied)n=sizeof(packet)-copied;
        memcpy(packet+copied,message->msg_iov[i].iov_base,n);copied+=n;
        if(copied==static_cast<size_t>(result))break;
    }
    if(copied!=static_cast<size_t>(result) || !authorized(fd,kind,packet,copied)) {
        closeRights(message);fprintf(stderr,"API32_TOMBSTONED_REQUEST_DENIED endpoint=%d\n",kind);
        errno=EACCES;return -1;
    }
    return result;
}
__attribute__((constructor)) static void validateScope() {
    char sdk[PROP_VALUE_MAX]={};__system_property_get("ro.build.version.sdk",sdk);
    if(syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL || strcmp(sdk,"32") ||
            getuid()!=1058 || access("/.pocket-readonly-system",F_OK))_exit(125);
    fprintf(stderr,"API32_TOMBSTONED_SCOPE_READY pid=%d uid=%u kernel_credentials=real profile_pid_start=checked\n",getpid(),getuid());
}
