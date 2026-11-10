// SPDX-License-Identifier: GPL-3.0-only
// Optional API32 external-file routing. File data remains on real AppFuse FDs;
// the trap only performs the authenticated metadata/open handshake.
#pragma once
#include "external_protocol.h"
#include <sys/time.h>
#include <sys/vfs.h>
static int external_files_enabled;
static int external_namespace_enabled;
static void external_root_path(char* out,const char* tail) {
    memcpy(out,pocket_loader_config.root,root_size);memcpy(out+root_size,tail,length(tail,PATH_LIMIT-root_size)+1);
}
static void external_files_start(void) {
    char path[1280];struct stat st;external_root_path(path,"/.pocket-external-enabled");
    external_files_enabled=raw(__NR_newfstatat,AT_FDCWD,(long)path,(long)&st,AT_SYMLINK_NOFOLLOW,0,0)==0 && S_ISREG(st.st_mode);
    external_root_path(path,"/.pocket-external-namespace-enabled");
    external_namespace_enabled=external_files_enabled && raw(__NR_newfstatat,AT_FDCWD,(long)path,(long)&st,AT_SYMLINK_NOFOLLOW,0,0)==0 && S_ISREG(st.st_mode);
}
static int external_file_path_policy(int dir,const char* original,const char* mapped,char* out,char* temporary,unsigned followFinal) {
    // MediaProvider's backing view must not recursively re-enter itself.
    if(external_storage_mode==3 && prefix(original,"/storage/emulated",17))return 0;
    if(prefix(original,"/storage/emulated",17) || prefix(original,"/mnt/user/0/emulated",20)) {
        size_t size=length(original,PATH_LIMIT);memcpy(out,original,size+1);return 1;
    }
    const char* alias=prefix(original,"/sdcard",7)?original+7:prefix(original,"/storage/self/primary",21)?original+21:0;
    if(alias) {
        if(external_storage_mode==3)return 0;
        size_t size=length(alias,PATH_LIMIT);if(size+20>=PATH_LIMIT)return -ENAMETOOLONG;
        memcpy(out,"/storage/emulated/0",19);memcpy(out+19,alias,size+1);return 1;
    }
    // The volume service prepares backing directories before a FUSE session
    // exists. Its privileged lower accesses are not emulated mount accesses.
    if((!guest_uid[1] || guest_uid[1]==1000 || external_storage_mode==3) && prefix(original,"/data/media",11))return 0;
    if(prefix(original,"/proc",5) || prefix(original,"/dev",4) || prefix(original,"/sys",4) || prefix(original,"/system",7) || prefix(original,"/vendor",7) || prefix(original,"/apex",5))return 0;
    // Recover aliases and openat(dirfd,relative) from the kernel's O_PATH link.
    // No file data is read here. Following operations must route the actual
    // target, including a final symlink stored in the app's internal data.
    // Otherwise a local link to another package's lower path bypasses FUSE.
    // Metadata/mutation operations on the link itself keep O_NOFOLLOW.
    char resolved[PATH_LIMIT],target[PATH_LIMIT];
    const char* current=mapped;int currentDir=dir;
    const char* suffix=0;
    long fd;unsigned links=0;
    for(;;) {
        fd=raw(__NR_openat,currentDir,(long)current,O_PATH|O_CLOEXEC|(followFinal?0:O_NOFOLLOW),0,0,0);
        if(fd>=0 || !followFinal || fd!=-ENOENT)break;
        // A following O_PATH cannot resolve a dangling final link. Peel only
        // that link using its actual held inode, then resolve its target's
        // parent. This also protects O_CREAT through an internal dangling link.
        long link=raw(__NR_openat,currentDir,(long)current,O_PATH|O_CLOEXEC|O_NOFOLLOW,0,0,0);
        if(link<0)break;
        long n=raw(__NR_readlinkat,link,(long)"",(long)target,PATH_LIMIT,0,0);
        if(n<0){raw(__NR_close,link,0,0,0,0,0);break;}
        if(n==PATH_LIMIT){raw(__NR_close,link,0,0,0,0,0);return -ENAMETOOLONG;}
        target[n]=0;
        if(++links>40){raw(__NR_close,link,0,0,0,0,0);return -ELOOP;}
        if(target[0]=='/')memcpy(resolved,target,(size_t)n+1);
        else {
            char name[64];proc_fd_path((int)link,name);
            long size=raw(__NR_readlinkat,AT_FDCWD,(long)name,(long)resolved,PATH_LIMIT,0,0);
            if(size<0){raw(__NR_close,link,0,0,0,0,0);return size;}
            if(size==PATH_LIMIT){raw(__NR_close,link,0,0,0,0,0);return -ENAMETOOLONG;}
            while(size && resolved[size-1]!='/')size--;
            if(!size || (size_t)size+(size_t)n>=PATH_LIMIT){raw(__NR_close,link,0,0,0,0,0);return -ENAMETOOLONG;}
            memcpy(resolved+size,target,(size_t)n+1);
        }
        raw(__NR_close,link,0,0,0,0,0);
        current=resolved;currentDir=AT_FDCWD;
    }
    if(fd<0) {
        if(current[0]!='/') {
            // A relative create can target a missing child or missing parent.
            // Resolve the held directory, not the nonexistent final pathname.
            fd=raw(__NR_openat,currentDir,(long)".",O_PATH|O_DIRECTORY|O_CLOEXEC,0,0,0);suffix=current;
        }else {
            size_t end=length(current,PATH_LIMIT);while(end && current[end-1]!='/')end--;
            if(!end)return 0;
            size_t parent=end==1?1:end-1;memcpy(out,current,parent);out[parent]=0;suffix=current+end;
            fd=raw(__NR_openat,currentDir,(long)out,O_PATH|O_DIRECTORY|O_CLOEXEC,0,0,0);
        }
        if(fd<0)return 0;
    }
    char link[64];proc_fd_path(fd,link);
    long n=raw(__NR_readlinkat,AT_FDCWD,(long)link,(long)temporary,PATH_LIMIT-1,0,0);raw(__NR_close,fd,0,0,0,0,0);
    if(n<0)return 0;
    if(suffix) {
        size_t size=length(suffix,PATH_LIMIT);
        if((size_t)n+size+1>=PATH_LIMIT)return -ENAMETOOLONG;
        if(n && temporary[n-1]!='/')temporary[n++]='/';
        memcpy(temporary+n,suffix,size);n+=(long)size;
    }
    temporary[n]=0;
    const char* lower=data_alias?"/system-data/media":"/data/media";
    size_t base=length(lower,32);
    if(!prefix(temporary,pocket_loader_config.root,root_size) || !prefix(temporary+root_size,lower,base))return 0;
    if(external_storage_mode==3 || !guest_uid[1] || guest_uid[1]==1000)return 0;
    size_t tail=(size_t)n-root_size-base;if(tail+17>=PATH_LIMIT)return -ENAMETOOLONG;
    memcpy(out,"/storage/emulated",17);memcpy(out+17,temporary+root_size+base,tail+1);return 1;
}
static int external_file_path(int dir,const char* original,const char* mapped,char* out,char* temporary) {
    return external_file_path_policy(dir,original,mapped,out,temporary,1);
}
static int external_file_path_nofollow(int dir,const char* original,const char* mapped,char* out,char* temporary) {
    return external_file_path_policy(dir,original,mapped,out,temporary,0);
}
static long external_connect(char* scratch) {
    char* file=scratch;external_root_path(file,"/.pocket-external-ready");
    struct PocketExternalLocator locator;
    long fd=raw(__NR_openat,AT_FDCWD,(long)file,O_RDONLY|O_CLOEXEC|O_NOFOLLOW,0,0,0);
    // Before mount there is no emulated user path. Match an absent pathname;
    // Java canonicalization handles ENOENT while propagating device errors.
    if(fd<0)return fd;
    long n=raw(__NR_read,fd,(long)&locator,sizeof(locator),0,0,0);raw(__NR_close,fd,0,0,0,0,0);
    if(n!=sizeof(locator) || locator.magic!=POCKET_EXTERNAL_LOCATOR_MAGIC || locator.version!=1 || !locator.pid || locator.pid>INT32_MAX || locator.reserved || !locator.start)return -EPROTO;
    struct sockaddr_un address;memset(&address,0,sizeof(address));address.sun_family=AF_UNIX;
    external_root_path(file,"/dev/socket/pocket_efs");size_t size=length(file,PATH_LIMIT);
    if(size>=sizeof(address.sun_path))return -ENAMETOOLONG;
    memcpy(address.sun_path,file,size+1);
    long socket=raw(__NR_socket,AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,0,0,0);if(socket<0)return socket;
    struct timeval deadline={15,0};raw(__NR_setsockopt,socket,SOL_SOCKET,SO_RCVTIMEO,(long)&deadline,sizeof(deadline),0);raw(__NR_setsockopt,socket,SOL_SOCKET,SO_SNDTIMEO,(long)&deadline,sizeof(deadline),0);
    long result=raw(__NR_connect,socket,(long)&address,offsetof(struct sockaddr_un,sun_path)+size+1,0,0,0);
    if(result<0)goto done;
    struct ucred peer;unsigned peer_size=sizeof(peer);
    result=raw(__NR_getsockopt,socket,SOL_SOCKET,SO_PEERCRED,(long)&peer,(long)&peer_size,0);
    if(result<0)goto done;
    if(peer_size!=sizeof(peer) || peer.pid!=(int)locator.pid || peer.uid!=(unsigned)raw(__NR_getuid,0,0,0,0,0,0)){result=-EACCES;goto done;}
    memcpy(file,"/proc/",6);size_t at=6+proc_number(file+6,locator.pid);memcpy(file+at,"/stat",6);
    fd=raw(__NR_openat,AT_FDCWD,(long)file,O_RDONLY|O_CLOEXEC|O_NOFOLLOW,0,0,0);
    if(fd<0){result=-EACCES;goto done;}
    n=raw(__NR_read,fd,(long)scratch,PATH_LIMIT,0,0,0);raw(__NR_close,fd,0,0,0,0,0);
    if(n<=0 || pocket_process_start(scratch,n)!=locator.start){result=-EACCES;goto done;}
    return socket;
done:raw(__NR_close,socket,0,0,0,0,0);return result;
}
static unsigned external_carrier_kind(int fd) {
    struct sockaddr_un address;memset(&address,0,sizeof(address));unsigned size=sizeof(address);
    if(raw(__NR_getsockname,fd,(long)&address,(long)&size,0,0,0)<0 || address.sun_family!=AF_UNIX || address.sun_path[0])return 0;
    unsigned tag=0,kind=0;
    if(size==offsetof(struct sockaddr_un,sun_path)+1+POCKET_EXTERNAL_DIRECTORY_TAG_SIZE+sizeof(uint64_t) && equal(address.sun_path+1,POCKET_EXTERNAL_DIRECTORY_TAG,POCKET_EXTERNAL_DIRECTORY_TAG_SIZE)){tag=POCKET_EXTERNAL_DIRECTORY_TAG_SIZE;kind=POCKET_EXTERNAL_OPEN_DIRECTORY;}
    else if(size==offsetof(struct sockaddr_un,sun_path)+1+POCKET_EXTERNAL_PATH_TAG_SIZE+sizeof(uint64_t) && equal(address.sun_path+1,POCKET_EXTERNAL_PATH_TAG,POCKET_EXTERNAL_PATH_TAG_SIZE)){tag=POCKET_EXTERNAL_PATH_TAG_SIZE;kind=POCKET_EXTERNAL_OPEN_PATH;}
    if(!kind)return 0;
    uint64_t declared=0,cookie=0;memcpy(&declared,address.sun_path+1+tag,sizeof(declared));unsigned length=sizeof(cookie);
    return raw(__NR_getsockopt,fd,SOL_SOCKET,SO_COOKIE,(long)&cookie,(long)&length,0)==0 && length==sizeof(cookie) && cookie && cookie==declared?kind:0;
}
static int external_directory_fd(int fd){return external_carrier_kind(fd)!=0;}
static long external_descriptor_rpc(const char* path,unsigned flags,unsigned mode,unsigned mask,int directory,unsigned operation,char* scratch) {
    long socket=external_connect(scratch);if(socket<0)return socket;
    long result=-EPROTO,n;
    struct PocketExternalRequest2 request={{POCKET_EXTERNAL_OPEN_MAGIC,external_namespace_enabled?2:1,operation==POCKET_EXT_DIR_SEARCH?0:flags,(uint32_t)raw(__NR_gettid,0,0,0,0,0,0),(uint32_t)length(path,PATH_LIMIT),operation,{0,directory>=0 && operation==POCKET_EXT_OPEN?POCKET_EXTERNAL_AT_FIRST:0}},mode,mask,0};
    const size_t header=external_namespace_enabled?sizeof(request):sizeof(request.header);
    struct iovec parts[2]={{&request,header},{(void*)path,request.header.path_size}};
    char proof[CMSG_SPACE(sizeof(int))];struct msghdr message;memset(&message,0,sizeof(message));message.msg_iov=parts;message.msg_iovlen=2;
    if(directory>=0){memset(proof,0,sizeof(proof));message.msg_control=proof;message.msg_controllen=sizeof(proof);struct cmsghdr* c=CMSG_FIRSTHDR(&message);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&directory,sizeof(directory));}
    n=raw(__NR_sendmsg,socket,(long)&message,MSG_NOSIGNAL,0,0,0);
    if(n<0 || (size_t)n!=header+request.header.path_size){result=n<0?n:-EIO;goto done;}
    struct PocketExternalReply reply;char control[CMSG_SPACE(8*sizeof(int))];struct iovec part={&reply,sizeof(reply)};
    memset(&message,0,sizeof(message));message.msg_iov=&part;message.msg_iovlen=1;message.msg_control=control;message.msg_controllen=sizeof(control);
    do{n=raw(__NR_recvmsg,socket,(long)&message,MSG_CMSG_CLOEXEC,0,0,0);}while(n==-EINTR);
    int rights[8],count=0,valid=n>0 && !(message.msg_flags&(MSG_TRUNC|MSG_CTRUNC));
    if(n<0){result=n;goto done;}
    for(size_t offset=0;offset+sizeof(struct cmsghdr)<=message.msg_controllen;) {
        struct cmsghdr* c=(struct cmsghdr*)(control+offset);
        if(c->cmsg_len<CMSG_LEN(0) || c->cmsg_len>message.msg_controllen-offset){valid=0;break;}
        offset+=(c->cmsg_len+sizeof(size_t)-1)&~(sizeof(size_t)-1);
        if(c->cmsg_level!=SOL_SOCKET || c->cmsg_type!=SCM_RIGHTS){valid=0;continue;}
        size_t bytes=c->cmsg_len-CMSG_LEN(0);if(bytes%sizeof(int))valid=0;
        for(size_t i=0;i+sizeof(int)<=bytes;i+=sizeof(int)) {
            int value;memcpy(&value,(char*)CMSG_DATA(c)+i,sizeof(value));if(count<8)rights[count++]=value;else{raw(__NR_close,value,0,0,0,0,0);valid=0;}
        }
    }
    if(valid && (size_t)n<sizeof(reply)) {
        size_t used=(size_t)n;while(used<sizeof(reply)) {
            n=raw(__NR_read,socket,(long)((char*)&reply+used),sizeof(reply)-used,0,0,0);
            if(n==-EINTR)continue;
            if(n<=0){valid=0;break;}used+=(size_t)n;
        }
    }
    result=-EPROTO;
    if(valid && !reply.reserved && reply.error<0 && reply.error>=-4095 && !count)result=reply.error;
    else if(valid && !reply.error && count==1 && (!reply.reserved || (external_namespace_enabled && (reply.reserved==POCKET_EXTERNAL_OPEN_DIRECTORY || reply.reserved==POCKET_EXTERNAL_OPEN_PATH) && external_carrier_kind(rights[0])==reply.reserved))) {
        long access=raw(__NR_fcntl,rights[0],F_GETFL,0,0,0,0);
        struct stat search;int searchValid=operation==POCKET_EXT_DIR_SEARCH && !reply.reserved && access>=0 && (access&O_PATH) && raw(__NR_fstat,rights[0],(long)&search,0,0,0,0)==0 && S_ISDIR(search.st_mode);
        if(operation==POCKET_EXT_DIR_SEARCH?searchValid:access>=0 && (reply.reserved==POCKET_EXTERNAL_OPEN_PATH?(flags&O_PATH)!=0:(flags&O_PATH)?0:reply.reserved?(flags&O_ACCMODE)==O_RDONLY:(access&O_ACCMODE)==(flags&O_ACCMODE))) {
            result=raw(__NR_fcntl,rights[0],F_SETFD,(flags&O_CLOEXEC)?FD_CLOEXEC:0,0,0,0);
            if(!result){result=rights[0];count=0;}
        }
    }
    for(int i=0;i<count;i++)raw(__NR_close,rights[i],0,0,0,0,0);
done:raw(__NR_close,socket,0,0,0,0,0);return result;
}
static long external_open_at_fd2(const char* path,unsigned flags,unsigned mode,unsigned mask,int directory,char* scratch) {
    return external_descriptor_rpc(path,flags,mode,mask,directory,POCKET_EXT_OPEN,scratch);
}
static long external_open_fd2(const char* path,unsigned flags,unsigned mode,unsigned mask,char* scratch) {
    return external_open_at_fd2(path,flags,mode,mask,-1,scratch);
}
static long external_open_fd(const char* path,unsigned flags,char* scratch) {
    return external_open_fd2(path,flags,0600,0077,scratch);
}
static long external_statfs_result(long socket,uintptr_t output) {
    long result=0;
    struct PocketExternalStatfsReply reply;size_t used=0;
    while(used<sizeof(reply)) {
        result=raw(__NR_read,socket,(long)((char*)&reply+used),sizeof(reply)-used,0,0,0);
        if(result==-EINTR)continue;
        if(result<=0){if(!result)result=-ECONNRESET;goto done;}used+=(size_t)result;
    }
    result=-EPROTO;
    if(reply.reserved || reply.error>0 || reply.error<-4095)goto done;
    if(reply.error){result=reply.error;goto done;}
    if(!reply.bsize || reply.bsize>UINT32_MAX || !reply.frsize || reply.frsize>UINT32_MAX || !reply.namelen || reply.namelen>UINT32_MAX || reply.bfree>reply.blocks || reply.bavail>reply.bfree || reply.ffree>reply.files)goto done;
    // These are the real provider's FUSE_STATFS fields. Logical mount flags
    // and fsid remain unset, as in the FUSE statfs payload itself.
    struct statfs value;memset(&value,0,sizeof(value));value.f_type=0x65735546;
    value.f_bsize=reply.bsize;value.f_blocks=reply.blocks;value.f_bfree=reply.bfree;value.f_bavail=reply.bavail;
    value.f_files=reply.files;value.f_ffree=reply.ffree;value.f_namelen=reply.namelen;value.f_frsize=reply.frsize;
    result=copy_to_user(output,(const char*)&value,sizeof(value));
    if(result>=0)result=0;
done:return result;
}
static long external_statfs_rpc(unsigned inode,uintptr_t output,char* scratch) {
    long socket=external_connect(scratch);if(socket<0)return socket;
    struct PocketExternalOpen request={POCKET_EXTERNAL_OPEN_MAGIC,1,0,(uint32_t)raw(__NR_gettid,0,0,0,0,0,0),0,1,{inode,0}};
    struct iovec part={&request,sizeof(request)};struct msghdr message;memset(&message,0,sizeof(message));message.msg_iov=&part;message.msg_iovlen=1;
    long result=raw(__NR_sendmsg,socket,(long)&message,MSG_NOSIGNAL,0,0,0);
    if(result!=sizeof(request)){if(result>=0)result=-EIO;goto done;}
    result=external_statfs_result(socket,output);
done:raw(__NR_close,socket,0,0,0,0,0);return result;
}
static long external_statfs_fd_rpc(int fd,uintptr_t output,char* scratch) {
    long socket=external_connect(scratch);if(socket<0)return socket;
    struct PocketExternalRequest2 request={{POCKET_EXTERNAL_OPEN_MAGIC,2,0,(uint32_t)raw(__NR_gettid,0,0,0,0,0,0),0,POCKET_EXT_STATFS,{0,0}},0,0,0};
    char control[CMSG_SPACE(sizeof(int))];memset(control,0,sizeof(control));
    struct iovec part={&request,sizeof(request)};struct msghdr message;memset(&message,0,sizeof(message));
    message.msg_iov=&part;message.msg_iovlen=1;message.msg_control=control;message.msg_controllen=sizeof(control);
    struct cmsghdr* rights=CMSG_FIRSTHDR(&message);rights->cmsg_level=SOL_SOCKET;rights->cmsg_type=SCM_RIGHTS;
    rights->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(rights),&fd,sizeof(fd));
    long result=raw(__NR_sendmsg,socket,(long)&message,MSG_NOSIGNAL,0,0,0);
    if(result==(long)sizeof(request))result=external_statfs_result(socket,output);
    else if(result>=0)result=-EIO;
    raw(__NR_close,socket,0,0,0,0,0);return result;
}

static int external_statfs_dispatch(long nr,__u64* r) {
    if(!external_files_enabled || nr!=__NR_fstatfs)return 0;
    long memory=raw(__NR_mmap,0,PATH_LIMIT,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(memory<0){r[0]=memory;return 1;}
    char* scratch=(char*)memory;
    if(external_namespace_enabled && external_carrier_kind((int)r[0])) {
        r[0]=external_statfs_fd_rpc((int)r[0],r[1],scratch);
        raw(__NR_munmap,memory,PATH_LIMIT,0,0,0,0);return 1;
    }
    external_root_path(scratch,"/.pocket-external-device");uint64_t device=0;
    long file=raw(__NR_openat,AT_FDCWD,(long)scratch,O_RDONLY|O_CLOEXEC|O_NOFOLLOW,0,0,0),count=-1;
    if(file>=0){count=raw(__NR_read,file,(long)&device,sizeof(device),0,0,0);raw(__NR_close,file,0,0,0,0,0);}
    struct stat st;int handled=0;
    if(count==sizeof(device) && device && raw(__NR_fstat,r[0],(long)&st,0,0,0,0)==0 && st.st_dev==device && st.st_ino>=3 && st.st_ino<=INT32_MAX) {
        r[0]=external_statfs_rpc((unsigned)st.st_ino,r[1],scratch);handled=1;
    }
    raw(__NR_munmap,memory,PATH_LIMIT,0,0,0,0);return handled;
}
static int external_open_dispatch(long nr,__u64* r,const char* original,const char* mapped,char* scratch) {
    if(!external_files_enabled || nr!=__NR_openat)return 0;
    char* path=scratch;unsigned flags=(unsigned)r[2];
    unsigned follow=!(flags&O_NOFOLLOW) && ((flags&O_PATH) || (flags&(O_CREAT|O_EXCL))!=(O_CREAT|O_EXCL));
    int match=follow?external_file_path((int)r[0],original,mapped,path,scratch+PATH_LIMIT):
        external_file_path_nofollow((int)r[0],original,mapped,path,scratch+PATH_LIMIT);
    if(!match)return 0;
    if(match<0){r[0]=match;return 1;}
    r[0]=external_open_fd(path,(unsigned)r[2],scratch+2*PATH_LIMIT);return 1;
}
