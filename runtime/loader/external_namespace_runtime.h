// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Included after external_runtime.h. This additional view is enabled only
// when its complete namespace transport is explicitly selected by Manager.
static long external_namespace_rpc_at(unsigned operation,unsigned flags,unsigned mode,unsigned mask,uint64_t size,const char* path,const char* second,int descriptor,int other,unsigned bases,uintptr_t output,char* scratch) {
    long socket=external_connect(scratch);if(socket<0)return socket;
    struct PocketExternalRequest2 request={{POCKET_EXTERNAL_OPEN_MAGIC,2,flags,(uint32_t)raw(__NR_gettid,0,0,0,0,0,0),path?(uint32_t)length(path,PATH_LIMIT):0,operation,{second?(uint32_t)length(second,PATH_LIMIT):0,bases}},mode,mask,size};
    struct iovec parts[3]={{&request,sizeof(request)},{(void*)path,request.header.path_size},{(void*)second,request.header.reserved[0]}};
    int values[2];unsigned count=0;if(descriptor>=0)values[count++]=descriptor;if(other>=0)values[count++]=other;
    char control[CMSG_SPACE(2*sizeof(int))];struct msghdr message;memset(&message,0,sizeof(message));message.msg_iov=parts;message.msg_iovlen=3;
    if(count) {
        memset(control,0,sizeof(control));message.msg_control=control;message.msg_controllen=CMSG_SPACE(count*sizeof(int));
        struct cmsghdr* rights=CMSG_FIRSTHDR(&message);rights->cmsg_level=SOL_SOCKET;rights->cmsg_type=SCM_RIGHTS;rights->cmsg_len=CMSG_LEN(count*sizeof(int));memcpy(CMSG_DATA(rights),values,count*sizeof(int));
    }
    long result=raw(__NR_sendmsg,socket,(long)&message,MSG_NOSIGNAL,0,0,0);
    if(result!=(long)(sizeof(request)+request.header.path_size+request.header.reserved[0])){if(result>=0)result=-EIO;goto done;}
    struct PocketExternalNamespaceReply reply;size_t used=0;
    while(used<sizeof(reply)) {
        result=raw(__NR_read,socket,(long)((char*)&reply+used),sizeof(reply)-used,0,0,0);
        if(result==-EINTR)continue;
        if(result<=0){if(!result)result=-ECONNRESET;goto done;}used+=(size_t)result;
    }
    if(reply.reserved || reply.error>0 || reply.error<-4095){result=-EPROTO;goto done;}
    result=reply.error;if(result<0)goto done;
    if(operation==POCKET_EXT_STAT || operation==POCKET_EXT_FSTAT) {
        const struct PocketExternalAttributes* a=&reply.attributes;
        if(!a->device || !(a->mode&S_IFMT) || a->blksize>INT32_MAX || a->atimensec>=1000000000 || a->mtimensec>=1000000000 || a->ctimensec>=1000000000){result=-EPROTO;goto done;}
        if(a->size>INT64_MAX || a->blocks>INT64_MAX){result=-EOVERFLOW;goto done;}
        struct stat st;memset(&st,0,sizeof(st));
        st.st_dev=a->device;st.st_ino=a->inode;st.st_size=a->size;st.st_blocks=a->blocks;
        st.st_mode=a->mode;st.st_nlink=a->nlink;st.st_uid=a->uid;st.st_gid=a->gid;st.st_blksize=a->blksize;
        st.st_atim.tv_sec=(int64_t)a->atime;st.st_atim.tv_nsec=a->atimensec;
        st.st_mtim.tv_sec=(int64_t)a->mtime;st.st_mtim.tv_nsec=a->mtimensec;
        st.st_ctim.tv_sec=(int64_t)a->ctime;st.st_ctim.tv_nsec=a->ctimensec;
        result=copy_to_user(output,(const char*)&st,sizeof(st));if(result>=0)result=0;
    }
done:raw(__NR_close,socket,0,0,0,0,0);return result;
}
static long external_namespace_rpc(unsigned operation,unsigned flags,unsigned mode,unsigned mask,uint64_t size,const char* path,const char* second,int descriptor,uintptr_t output,char* scratch) {
    return external_namespace_rpc_at(operation,flags,mode,mask,size,path,second,descriptor,-1,0,output,scratch);
}
static long external_directory_rpc(unsigned operation,unsigned flags,uint64_t value,int fd,uintptr_t output,char* scratch) {
    long socket=external_connect(scratch);if(socket<0)return socket;
    struct PocketExternalRequest2 request={{POCKET_EXTERNAL_OPEN_MAGIC,2,flags,(uint32_t)raw(__NR_gettid,0,0,0,0,0,0),0,operation,{0,0}},0,0,value};
    char control[CMSG_SPACE(sizeof(int))];memset(control,0,sizeof(control));struct iovec part={&request,sizeof(request)};struct msghdr message;memset(&message,0,sizeof(message));
    message.msg_iov=&part;message.msg_iovlen=1;message.msg_control=control;message.msg_controllen=sizeof(control);
    struct cmsghdr* rights=CMSG_FIRSTHDR(&message);rights->cmsg_level=SOL_SOCKET;rights->cmsg_type=SCM_RIGHTS;rights->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(rights),&fd,sizeof(fd));
    long result=raw(__NR_sendmsg,socket,(long)&message,MSG_NOSIGNAL,0,0,0),memory=-1;size_t used=0;
    if(result!=sizeof(request)){if(result>=0)result=-EIO;goto done;}
    struct PocketExternalDirectoryReply reply;
    while(used<sizeof(reply)) {
        result=raw(__NR_read,socket,(long)((char*)&reply+used),sizeof(reply)-used,0,0,0);if(result==-EINTR)continue;
        if(result<=0){if(!result)result=-ECONNRESET;goto done;}used+=(size_t)result;
    }
    if(reply.error>0 || reply.error<-4095 || (reply.error && reply.size)){result=-EPROTO;goto done;}
    result=reply.error;if(result<0)goto done;
    if(reply.value>INT64_MAX || (operation!=POCKET_EXT_DIR_READ && operation!=POCKET_EXT_FD_LINK && reply.size) || (operation==POCKET_EXT_FD_LINK && reply.value)){result=-EPROTO;goto done;}
    if(operation!=POCKET_EXT_DIR_READ && operation!=POCKET_EXT_FD_LINK){result=(long)reply.value;goto done;}
    if(reply.size>flags || reply.size>65536){result=-EPROTO;goto done;}
    if(reply.size) {
        memory=raw(__NR_mmap,0,reply.size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(memory<0){result=memory;goto done;}
        used=0;while(used<reply.size) {
            result=raw(__NR_read,socket,memory+used,reply.size-used,0,0,0);if(result==-EINTR)continue;
            if(result<=0){if(!result)result=-ECONNRESET;goto done;}used+=(size_t)result;
        }
    }
    result=0;
    if(output>UINTPTR_MAX-reply.size)result=-EFAULT;
    for(used=0;!result && used<reply.size;) {
        size_t chunk=reply.size-used;if(chunk>4096)chunk=4096;
        long copied=copy_to_user(output+used,(const char*)(memory+used),chunk);
        if(copied!=(long)chunk)result=copied<0?copied:-EFAULT;else used+=chunk;
    }
    if(operation==POCKET_EXT_DIR_READ) {
        unsigned char ack=result<0?0:1;
        long sent=raw(__NR_sendto,socket,(long)&ack,1,MSG_NOSIGNAL,0,0);
        if(result>=0)result=sent==1?(long)reply.size:sent<0?sent:-EIO;
    }else if(!result)result=reply.size;
done:
    if(memory>=0)raw(__NR_munmap,memory,reply.size,0,0,0,0);
    raw(__NR_close,socket,0,0,0,0,0);return result;
}
static long external_directory_chdir(int fd,char* scratch) {
    long anchor=external_descriptor_rpc("",O_PATH|O_DIRECTORY|O_CLOEXEC,0,0,fd,POCKET_EXT_DIR_SEARCH,scratch);
    if(anchor<0)return anchor;
    long result=raw(__NR_fchdir,anchor,0,0,0,0,0);raw(__NR_close,anchor,0,0,0,0,0);return result;
}
static int external_directory_fd_dispatch(long nr,__u64* r) {
    if(!external_namespace_enabled || external_kernel_active())return 0;
    int fd=(int)r[0];
    switch(nr) {
    case __NR_getdents64:case __NR_lseek:case __NR_fstat:case __NR_ftruncate:case __NR_fcntl:case __NR_fchdir:
    case __NR_fsync:case __NR_fdatasync:case __NR_ioctl:case __NR_fchmod:case __NR_fchown:
    case __NR_fgetxattr:case __NR_fsetxattr:case __NR_flistxattr:case __NR_fremovexattr:
    case __NR_read:case __NR_readv:case __NR_pread64:case __NR_preadv:case __NR_preadv2:
    case __NR_write:case __NR_writev:case __NR_pwrite64:case __NR_pwritev:case __NR_pwritev2:
    case __NR_getsockname:case __NR_getpeername:case __NR_getsockopt:case __NR_setsockopt:
    case __NR_bind:case __NR_connect:case __NR_listen:case __NR_accept:case __NR_accept4:case __NR_shutdown:
    case __NR_sendto:case __NR_sendmsg:case __NR_sendmmsg:case __NR_recvfrom:case __NR_recvmsg:case __NR_recvmmsg:break;
    case __NR_mmap:if(r[3]&MAP_ANONYMOUS)return 0;fd=(int)r[4];break;
    default:return 0;
    }
    unsigned kind=external_carrier_kind(fd);if(!kind)return 0;
    if(kind==POCKET_EXTERNAL_OPEN_PATH) {
        if(nr==__NR_fcntl && (r[1]==F_GETFD || r[1]==F_SETFD || r[1]==F_DUPFD || r[1]==F_DUPFD_CLOEXEC))return 0;
        if(nr!=__NR_fstat && nr!=__NR_fchdir && !(nr==__NR_fcntl && r[1]==F_GETFL)){r[0]=(uint64_t)-EBADF;return 1;}
    }
    if(nr==__NR_fcntl && r[1]!=F_GETFL && r[1]!=F_SETFL)return 0;
    if(nr==__NR_read || nr==__NR_readv || nr==__NR_pread64 || nr==__NR_preadv || nr==__NR_preadv2 || nr==__NR_write || nr==__NR_writev || nr==__NR_pwrite64 || nr==__NR_pwritev || nr==__NR_pwritev2 || nr==__NR_ftruncate){r[0]=(uint64_t)-EISDIR;return 1;}
    if(nr==__NR_ioctl){r[0]=(uint64_t)-ENOTTY;return 1;}
    if(nr==__NR_mmap){r[0]=(uint64_t)-ENODEV;return 1;}
    if(nr==__NR_fchmod || nr==__NR_fchown){r[0]=(uint64_t)-EPERM;return 1;}
    if(nr==__NR_fgetxattr || nr==__NR_fsetxattr || nr==__NR_flistxattr || nr==__NR_fremovexattr){r[0]=(uint64_t)-EOPNOTSUPP;return 1;}
    if(nr!=__NR_getdents64 && nr!=__NR_lseek && nr!=__NR_fstat && nr!=__NR_fcntl && nr!=__NR_fsync && nr!=__NR_fdatasync && nr!=__NR_fchdir){r[0]=(uint64_t)-ENOTSOCK;return 1;}
    long memory=raw(__NR_mmap,0,PATH_LIMIT,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(memory<0){r[0]=memory;return 1;}
    if(nr==__NR_fchdir)r[0]=external_directory_chdir(fd,(char*)memory);
    else if(nr==__NR_getdents64)r[0]=external_directory_rpc(POCKET_EXT_DIR_READ,r[2]>65536?65536:(unsigned)r[2],0,fd,r[1],(char*)memory);
    else if(nr==__NR_lseek)r[0]=external_directory_rpc(POCKET_EXT_DIR_SEEK,(unsigned)r[2],r[1],fd,0,(char*)memory);
    else if(nr==__NR_fstat)r[0]=external_namespace_rpc(POCKET_EXT_FSTAT,0,0,0,0,0,0,fd,r[1],(char*)memory);
    else if(nr==__NR_fcntl)r[0]=external_directory_rpc(POCKET_EXT_DIR_FLAGS,r[1]==F_SETFL,r[1]==F_SETFL?r[2]:0,fd,0,(char*)memory);
    else r[0]=external_directory_rpc(POCKET_EXT_DIR_SYNC,nr==__NR_fdatasync,0,fd,0,(char*)memory);
    raw(__NR_munmap,memory,PATH_LIMIT,0,0,0,0);return 1;
}
static long external_current_umask(char* scratch) {
    // Query /proc rather than temporarily changing the process-wide mask:
    // another Java/native thread can create a file concurrently with this trap.
    long fd=raw(__NR_openat,AT_FDCWD,(long)"/proc/self/status",O_RDONLY|O_CLOEXEC,0,0,0);if(fd<0)return fd;
    long n=raw(__NR_read,fd,(long)scratch,PATH_LIMIT-1,0,0,0);raw(__NR_close,fd,0,0,0,0,0);if(n<0)return n;
    scratch[n]=0;
    for(long at=0;at+6<n;at++)if((!at || scratch[at-1]=='\n') && equal(scratch+at,"Umask:",6)) {
        at+=6;while(at<n && (scratch[at]==' ' || scratch[at]=='\t'))at++;
        unsigned mask=0,digits=0;while(at<n && scratch[at]>='0' && scratch[at]<='7') {
            mask=mask*8+(unsigned)(scratch[at++]-'0');if(++digits>4 || mask>0777)return -EPROTO;
        }
        return digits && (at==n || scratch[at]=='\n')?(long)mask:-EPROTO;
    }
    return -EOPNOTSUPP;
}
static int external_namespace_proxy_fd(int fd,char* scratch) {
    external_root_path(scratch,"/.pocket-external-device");uint64_t device=0;
    long file=raw(__NR_openat,AT_FDCWD,(long)scratch,O_RDONLY|O_CLOEXEC|O_NOFOLLOW,0,0,0),count=-1;
    if(file>=0){count=raw(__NR_read,file,(long)&device,sizeof(device),0,0,0);raw(__NR_close,file,0,0,0,0,0);}
    struct stat st;
    return count==sizeof(device) && device && raw(__NR_fstat,fd,(long)&st,0,0,0,0)==0 && S_ISREG(st.st_mode) && st.st_dev==device;
}
static int external_namespace_fd_dispatch(long nr,__u64* r) {
    if(!external_namespace_enabled || external_kernel_active() || (nr!=__NR_fstat && nr!=__NR_ftruncate))return 0;
    if(external_directory_fd_dispatch(nr,r))return 1;
    long memory=raw(__NR_mmap,0,PATH_LIMIT,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(memory<0){r[0]=memory;return 1;}
    char* scratch=(char*)memory;int handled=0;
    if(external_namespace_proxy_fd((int)r[0],scratch)) {
        r[0]=external_namespace_rpc(nr==__NR_fstat?POCKET_EXT_FSTAT:POCKET_EXT_FTRUNCATE,0,0,0,nr==__NR_ftruncate?r[1]:0,0,0,(int)r[0],nr==__NR_fstat?r[1]:0,scratch);handled=1;
    }
    raw(__NR_munmap,memory,PATH_LIMIT,0,0,0,0);return handled;
}
// Only this process/current task FD namespace is an alias. The number is
// never a provider inode: a registered carrier must still travel by SCM_RIGHTS.
static int external_proc_fd_path(const char* path,const char** suffix) {
    const char* tail=0;size_t count=length(path,PATH_LIMIT);
    if(count>=14 && equal(path,"/proc/self/fd/",14))tail=path+14;
    else if(count>=21 && equal(path,"/proc/thread-self/fd/",21))tail=path+21;
    else if(count>6 && equal(path,"/proc/",6)) {
        const char* p=path+6;uint64_t pid=0;
        while(*p>='0' && *p<='9' && pid<=INT32_MAX)pid=pid*10+(unsigned)(*p++-'0');
        if(pid && (pid==(uint64_t)raw(__NR_getpid,0,0,0,0,0,0) || pid==(uint64_t)raw(__NR_gettid,0,0,0,0,0,0)) && length(p,PATH_LIMIT)>=4 && equal(p,"/fd/",4))tail=p+4;
    }
    if(!tail || !*tail || *tail<'0' || *tail>'9')return -1;
    uint64_t fd=0;while(*tail>='0' && *tail<='9' && fd<=INT32_MAX)fd=fd*10+(unsigned)(*tail++-'0');
    if(fd>INT32_MAX || (*tail && *tail!='/'))return -1;
    *suffix=tail;return (int)fd;
}
static int external_proc_directory_path(const char* path,const char** relative) {
    const char* tail=0;int fd=external_proc_fd_path(path,&tail);
    // A suffix follows the magic link even for a nofollow final operation.
    // Exact-link readlink/lstat/open semantics remain separate from a dir walk.
    if(fd<0 || !tail || *tail!='/' || !external_directory_fd(fd))return -1;
    *relative=tail[1]?tail+1:".";return fd;
}
static int external_namespace_path_dispatch(long nr,__u64* r,const char* original,const char* mapped,const char* secondMapped,char* scratch) {
    if(!external_namespace_enabled || external_kernel_active())return 0;
    if(nr==__NR_readlinkat || nr==__NR_newfstatat || nr==__NR_chdir || nr==__NR_openat) {
        const char* tail=0;int fd=external_proc_fd_path(original,&tail);
        if(fd>=0 && !*tail && (external_directory_fd(fd) || external_namespace_proxy_fd(fd,scratch))) {
            if(nr==__NR_openat) {
                unsigned flags=(unsigned)r[2];if(flags&O_PATH)flags&=O_PATH|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW;
                // Preserve proc's magic-link nofollow/exclusive semantics in
                // the kernel; a following reopen always proves the actual FD.
                if(!(flags&O_NOFOLLOW) && (flags&(O_CREAT|O_EXCL))!=(O_CREAT|O_EXCL)) {
                    r[0]=external_descriptor_rpc("",flags,0,0,fd,POCKET_EXT_FD_REOPEN,scratch);return 1;
                }
            }
            if(nr==__NR_readlinkat) {
                r[0]=!r[3] || r[3]>INT64_MAX?(uint64_t)-EINVAL:(uint64_t)external_directory_rpc(POCKET_EXT_FD_LINK,r[3]>4095?4095:(unsigned)r[3],0,fd,r[2],scratch);return 1;
            }
            if(nr==__NR_newfstatat && !(r[3]&AT_SYMLINK_NOFOLLOW)) {
                r[0]=r[3]&~(uint64_t)(AT_EMPTY_PATH|AT_NO_AUTOMOUNT)?(uint64_t)-EINVAL:
                    (uint64_t)external_namespace_rpc(POCKET_EXT_FSTAT,0,0,0,0,0,0,fd,r[2],scratch);return 1;
            }
            if(nr==__NR_chdir) {
                r[0]=external_directory_fd(fd)?(uint64_t)external_directory_chdir(fd,scratch):(uint64_t)-ENOTDIR;return 1;
            }
        }
    }
    if(nr==__NR_newfstatat && !original[0] && (r[3]&AT_EMPTY_PATH)) {
        if(r[3]&~(AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW|AT_NO_AUTOMOUNT)){r[0]=(uint64_t)-EINVAL;return 1;}
        __u64 descriptor[6]={r[0],r[2],0,0,0,0};
        if(external_namespace_fd_dispatch(__NR_fstat,descriptor)){r[0]=descriptor[0];return 1;}return 0;
    }
    int dir=(nr==__NR_truncate || nr==__NR_statfs || nr==__NR_chdir || nr==__NR_execve || nr==__NR_getxattr || nr==__NR_lgetxattr || nr==__NR_setxattr || nr==__NR_lsetxattr || nr==__NR_listxattr || nr==__NR_llistxattr || nr==__NR_removexattr || nr==__NR_lremovexattr)?AT_FDCWD:(int)r[nr==__NR_symlinkat?1:0];
    char* path=scratch;char* second=scratch+PATH_LIMIT;char* temporary=scratch+2*PATH_LIMIT;char* rpc=scratch+3*PATH_LIMIT;
    unsigned openFlags=(unsigned)r[2];if(nr==__NR_openat && (openFlags&O_PATH))openFlags&=O_PATH|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW;
    // Following operations, including unsupported mutations, must not escape
    // provider policy through a final link in an internal app directory.
    unsigned follow=1;
    switch(nr) {
    case __NR_openat:follow=!(openFlags&O_NOFOLLOW) && (openFlags&(O_CREAT|O_EXCL))!=(O_CREAT|O_EXCL);break;
    case __NR_newfstatat:case __NR_utimensat:follow=!(r[3]&AT_SYMLINK_NOFOLLOW);break;
    case __NR_fchownat:case __NR_execveat:follow=!(r[4]&AT_SYMLINK_NOFOLLOW);break;
    case __NR_linkat:follow=!!(r[4]&AT_SYMLINK_FOLLOW);break;
    case __NR_readlinkat:case __NR_mkdirat:case __NR_unlinkat:case __NR_renameat:case __NR_renameat2:
    case __NR_symlinkat:case __NR_mknodat:case __NR_lgetxattr:case __NR_lsetxattr:
    case __NR_llistxattr:case __NR_lremovexattr:follow=0;break;
    }
    int firstBase=dir>=0 && original[0] && original[0]!='/' && external_directory_fd(dir)?dir:-1,secondBase=-1;
    const char* firstRelative=original;
    if(firstBase<0)firstBase=external_proc_directory_path(original,&firstRelative);
    int match,other=0;
    if(firstBase>=0){memcpy(path,firstRelative,length(firstRelative,PATH_LIMIT)+1);match=1;}
    else match=follow?external_file_path(dir,original,mapped,path,temporary):
        external_file_path_nofollow(dir,original,mapped,path,temporary);
    if(nr==__NR_renameat || nr==__NR_renameat2 || nr==__NR_linkat) {
        long rc=copy_path(temporary,r[3]);if(rc<0){r[0]=rc;return 1;}
        secondBase=(int)r[2]>=0 && temporary[0] && temporary[0]!='/' && external_directory_fd((int)r[2])?(int)r[2]:-1;
        const char* secondRelative=temporary;
        if(secondBase<0)secondBase=external_proc_directory_path(temporary,&secondRelative);
        if(secondBase>=0){memcpy(second,secondRelative,length(secondRelative,PATH_LIMIT)+1);other=1;}
        else other=external_file_path_nofollow((int)r[2],temporary,secondMapped,second,rpc);
        if(!match && !other)return 0;
        if(match<0 || other<0){r[0]=match<0?match:other;return 1;}
        if(!match || !other){r[0]=(uint64_t)-EXDEV;return 1;}
    }else {if(!match)return 0;if(match<0){r[0]=match;return 1;}}
    long mask=0;
    if(nr==__NR_mkdirat || (nr==__NR_openat && (openFlags&O_CREAT))){mask=external_current_umask(rpc);if(mask<0){r[0]=mask;return 1;}}
    unsigned bases=(firstBase>=0?POCKET_EXTERNAL_AT_FIRST:0)|(secondBase>=0?POCKET_EXTERNAL_AT_SECOND:0);
    switch(nr) {
    case __NR_chdir: {
        long fd=external_open_at_fd2(path,O_PATH|O_DIRECTORY|O_CLOEXEC,0,0,firstBase,rpc);
        if(fd<0)r[0]=fd;
        else {r[0]=external_directory_chdir((int)fd,rpc);raw(__NR_close,fd,0,0,0,0,0);}
        break;
    }
    case __NR_openat:r[0]=external_open_at_fd2(path,openFlags,(openFlags&O_CREAT)?(unsigned)r[3]:0,(unsigned)mask,firstBase,rpc);break;
    case __NR_readlinkat:
        if(!r[3] || r[3]>INT64_MAX)r[0]=(uint64_t)-EINVAL;
        else {
            // Emulated-volume regular files/directories are not links. Still
            // resolve through the provider first to preserve absence/private
            // policy errors, including descendants of a held proc FD alias.
            long rc=external_namespace_rpc_at(POCKET_EXT_STAT,AT_SYMLINK_NOFOLLOW,0,0,0,path,0,firstBase,-1,bases,(uintptr_t)temporary,rpc);
            r[0]=rc<0?(uint64_t)rc:(uint64_t)-EINVAL;
        }break;
    case __NR_newfstatat:r[0]=external_namespace_rpc_at(POCKET_EXT_STAT,(unsigned)r[3],0,0,0,path,0,firstBase,-1,bases,r[2],rpc);break;
    case __NR_mkdirat:r[0]=external_namespace_rpc_at(POCKET_EXT_MKDIR,0,(unsigned)r[2],(unsigned)mask,0,path,0,firstBase,-1,bases,0,rpc);break;
    case __NR_unlinkat:r[0]=external_namespace_rpc_at(POCKET_EXT_REMOVE,(unsigned)r[2],0,0,0,path,0,firstBase,-1,bases,0,rpc);break;
    case __NR_renameat:case __NR_renameat2:r[0]=external_namespace_rpc_at(POCKET_EXT_RENAME,nr==__NR_renameat2?(unsigned)r[4]:0,0,0,0,path,second,firstBase,secondBase,bases,0,rpc);break;
    case __NR_truncate:r[0]=external_namespace_rpc(POCKET_EXT_TRUNCATE,0,0,0,r[1],path,0,-1,0,rpc);break;
    case __NR_faccessat:r[0]=external_namespace_rpc_at(POCKET_EXT_ACCESS,(unsigned)r[2],0,0,0,path,0,firstBase,-1,bases,0,rpc);break;
    default:r[0]=(uint64_t)-EOPNOTSUPP;break;
    }
    return 1;
}
