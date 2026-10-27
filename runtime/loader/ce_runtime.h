// SPDX-License-Identifier: GPL-3.0-only
// Credential-encrypted file transport. All plaintext file bytes stay in RAM;
// persistent guest names/content are owned by the Manager's encrypted VFS.
#include <linux/memfd.h>
static int ce_enabled;
static uint64_t ce_fuse_device;
static long ce_copy_out(uintptr_t to,const char* from,size_t size) {
    while(size){size_t n=size>4096?4096:size;long rc=copy_to_user(to,from,n);if(rc<0)return rc;to+=n;from+=n;size-=n;}return 0;
}
static long ce_copy_in(char* to,uintptr_t from,size_t size) {
    while(size){size_t n=size>4096?4096:size;long rc=copy_from_user(to,from,n);if(rc<0)return rc;to+=n;from+=n;size-=n;}return 0;
}
static unsigned ce_umask(char* scratch) {
    long fd=raw(__NR_openat,AT_FDCWD,(long)"/proc/self/status",O_RDONLY|O_CLOEXEC,0,0,0);if(fd<0)return 077;
    long size=raw(__NR_read,fd,(long)scratch,PATH_LIMIT-1,0,0,0);raw(__NR_close,fd,0,0,0,0,0);
    if(size<0)return 077;scratch[size]=0;
    for(long i=0;i+6<size;i++)if((i==0 || scratch[i-1]=='\n') && equal(scratch+i,"Umask:",6)) {
        size_t at=i+6;while(scratch[at]==' '||scratch[at]=='\t')at++;unsigned value=0;
        while(scratch[at]>='0'&&scratch[at]<='7')value=(value<<3)+(scratch[at++]-'0');return value&0777;
    }
    return 077;
}
struct CeRequest {uint32_t magic,version,op,nr,uid,gid;uint64_t a[6];uint32_t p,q,data,groups;};
struct CeReply {long value;unsigned size,count;int fd[2];char data[16384];};
static long ce_exact(long fd,char* bytes,size_t count,int writing) {
    while(count){long n=raw(writing?__NR_write:__NR_read,fd,(long)bytes,count,0,0,0);if(n==-EINTR)continue;if(n<=0)return n<0?n:-EIO;bytes+=n;count-=n;}return 0;
}
static void ce_decimal(char* out,unsigned value) {
    char reverse[16];unsigned n=0;do{reverse[n++]='0'+value%10;value/=10;}while(value);
    for(unsigned i=0;i<n;i++)out[i]=reverse[n-i-1];out[n]=0;
}
static long ce_fd_path(int fd,char* out) {
    char path[64]="/proc/self/fd/";ce_decimal(path+14,(unsigned)fd);
    long n=raw(__NR_readlinkat,AT_FDCWD,(long)path,(long)out,PATH_LIMIT-1,0,0);if(n>=0)out[n]=0;return n;
}
static int ce_descriptor(int fd,const char* path) {
    if(prefix(path,pocket_loader_config.root,root_size) && prefix(path+root_size,"/.pocket-ce-dirs",16))return 1;
    if(fd<0 || !ce_fuse_device || !prefix(path,"/mnt/appfuse",12))return 0;
    struct stat st;return raw(__NR_fstat,fd,(long)&st,0,0,0,0)==0 && st.st_dev==ce_fuse_device;
}
static long ce_rpc(struct CeRequest* h,const char* p,const char* q,const char* data,struct CeReply* reply) {
    h->magic=0x31454350;h->version=1;h->uid=guest_identity?guest_uid[1]:0;h->gid=guest_identity?guest_gid[1]:0;
    h->p=p?length(p,PATH_LIMIT):0;h->q=q?length(q,PATH_LIMIT):0;h->groups=guest_identity?guest_group_count:0;
    reply->fd[0]=reply->fd[1]=-1;reply->size=reply->count=0;
    struct sockaddr_un address={.sun_family=AF_UNIX};
    if(root_size+22>=sizeof(address.sun_path))return -ENAMETOOLONG;
    memcpy(address.sun_path,pocket_loader_config.root,root_size);memcpy(address.sun_path+root_size,"/dev/socket/pocket_ce",21);
    long socket=raw(__NR_socket,AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,0,0,0);if(socket<0)return socket;
    long rc=raw(__NR_connect,socket,(long)&address,offsetof(struct sockaddr_un,sun_path)+root_size+21,0,0,0);
    if(rc<0)goto done;
    rc=ce_exact(socket,(char*)h,sizeof(*h),1);if(rc<0)goto done;
    if(h->p){rc=ce_exact(socket,(char*)p,h->p,1);if(rc<0)goto done;}
    if(h->q){rc=ce_exact(socket,(char*)q,h->q,1);if(rc<0)goto done;}
    if(h->data){rc=ce_exact(socket,(char*)data,h->data,1);if(rc<0)goto done;}
    if(h->groups){rc=ce_exact(socket,(char*)guest_groups,h->groups*sizeof(unsigned),1);if(rc<0)goto done;}
    char tag=0,control[CMSG_SPACE(2*sizeof(int))];struct iovec iov={&tag,1};
    struct msghdr message={.msg_iov=&iov,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
    rc=raw(__NR_recvmsg,socket,(long)&message,MSG_CMSG_CLOEXEC,0,0,0);if(rc!=1 || (unsigned char)tag!=0xa5){rc=-EIO;goto done;}
    struct cmsghdr* c=CMSG_FIRSTHDR(&message);
    if(c && c->cmsg_len>=CMSG_LEN(0) && c->cmsg_len<=message.msg_controllen && c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS) {
        size_t count=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);if(count>2)count=2;memcpy(reply->fd,CMSG_DATA(c),count*sizeof(int));reply->count=count;
    }
    struct {int64_t result;uint32_t size,count;} result;
    rc=ce_exact(socket,(char*)&result,sizeof(result),0);if(rc<0)goto done;
    if(result.size>sizeof(reply->data) || result.count!=reply->count || (message.msg_flags&MSG_CTRUNC)){rc=-EIO;goto done;}
    reply->value=result.result;reply->size=result.size;
    rc=ce_exact(socket,reply->data,result.size,0);
done:raw(__NR_close,socket,0,0,0,0,0);
    if(rc<0)for(unsigned i=0;i<reply->count;i++)if(reply->fd[i]>=0){raw(__NR_close,reply->fd[i],0,0,0,0,0);reply->fd[i]=-1;}
    return rc<0?rc:reply->value;
}
static void ce_close_reply(struct CeReply* reply) {for(unsigned i=0;i<reply->count;i++)if(reply->fd[i]>=0)raw(__NR_close,reply->fd[i],0,0,0,0,0);}
static int ce_user_path(const char* path,const char* base) {
    size_t n=length(base,PATH_LIMIT);return prefix(path,base,n) && path[n]=='/' && path[n+1]>='0' && path[n+1]<='9';
}
static int ce_candidate(const char* path) {
    const char* p=path;
    if(prefix(p,pocket_loader_config.root,root_size)) {
        p+=root_size;if(prefix(p,"/.pocket-ce-dirs",16))return 1;
        if(prefix(p,"/system-data",12)){p+=12;return ce_user_path(p,"/user")||prefix(p,"/data",5)||ce_user_path(p,"/system_ce")||ce_user_path(p,"/misc_ce")||ce_user_path(p,"/media");}
    }
    if(ce_user_path(p,"/data/user") || ce_user_path(p,"/data/system_ce") || ce_user_path(p,"/data/misc_ce") || ce_user_path(p,"/data/media") || ce_user_path(p,"/storage/emulated"))return 1;
    if(ce_user_path(p,"/mnt/runtime/default/emulated") || ce_user_path(p,"/mnt/runtime/read/emulated") || ce_user_path(p,"/mnt/runtime/write/emulated"))return 1;
    if(ce_user_path(p,"/mnt/user")){const char* tail=p+10;while(*tail>='0'&&*tail<='9')tail++;if(prefix(tail,"/primary",8))return 1;}
    const char* paths[]={"/data/user/0","/data/data","/data/system_ce/0","/data/misc_ce/0","/data/media/0","/sdcard","/storage/emulated/0","/storage/self/primary","/mnt/user/0/primary","/mnt/runtime/default/emulated/0","/mnt/runtime/read/emulated/0","/mnt/runtime/write/emulated/0"};
    for(unsigned i=0;i<sizeof(paths)/sizeof(*paths);i++)if(prefix(p,paths[i],length(paths[i],PATH_LIMIT)))return 1;return 0;
}
static long ce_path(int dir,uintptr_t source,char* out,char* temporary) {
    long rc=copy_path(temporary,source);if(rc<0)return rc;
    if(temporary[0]=='/')memcpy(out,temporary,(size_t)rc+1);
    else {
        rc=dir==AT_FDCWD?raw(__NR_getcwd,(long)out,PATH_LIMIT,0,0,0,0):ce_fd_path(dir,out);if(rc<0)return rc;
        size_t a=length(out,PATH_LIMIT),b=length(temporary,PATH_LIMIT);if(a+b+2>PATH_LIMIT)return -ENAMETOOLONG;
        out[a++]='/';memcpy(out+a,temporary,b+1);
    }
    // Resolve proc magic-link prefixes before deciding whether a path belongs
    // to CE. Otherwise an absolute /proc/self/fd/<directory>/child alias could
    // escape the virtual namespace into the empty descriptor directory.
    if(equal(out,"/proc/",6)) {
        size_t at=6;while(out[at] && out[at]!='/')at++;
        if(out[at]=='/' && equal(out+at,"/fd/",4)) {
            at+=4;size_t first=at;while(out[at]>='0'&&out[at]<='9')at++;
            if(at==first || (out[at] && out[at]!='/'))return 0;
        } else if(out[at]=='/' && prefix(out+at,"/cwd",4) && (!out[at+4]||out[at+4]=='/'))at+=4;
        else return 0;
        char saved=out[at];out[at]=0;
        long n=raw(__NR_readlinkat,AT_FDCWD,(long)out,(long)temporary,PATH_LIMIT-1,0,0);out[at]=saved;
        if(n>=0) {
            temporary[n]=0;size_t tail=length(out+at,PATH_LIMIT);
            if(ce_candidate(temporary)) {
                if((size_t)n+tail+1>PATH_LIMIT)return -ENAMETOOLONG;
                memcpy(temporary+n,out+at,tail+1);memcpy(out,temporary,n+tail+1);
            }
        }
    }
    return 0;
}
static long ce_fd_request(long nr,int fd,__u64* r,struct CeReply* reply,char* path) {
    struct stat st;long rc=raw(__NR_fstat,fd,(long)&st,0,0,0,0);if(rc<0)return rc;
    struct CeRequest h={.op=2,.nr=(uint32_t)nr,.a={st.st_dev,st.st_ino,r[1],r[2],r[3],r[4]}};
    return ce_rpc(&h,path,0,0,reply);
}
struct CeDent {uint64_t ino;int64_t offset;unsigned short length;unsigned char type;char name[];};
// Hidden regular lock descriptors use an independent OFD offset to record
// their guest fd number. Kernel state survives fork/exec without shared locks
// in a signal handler. Each guest open owns a separate lock fd, so POSIX
// close-any-fd and CLOEXEC lock release rules are preserved by the kernel.
static int ce_find_lock(int original,char* scratch) {
    long dir=raw(__NR_openat,AT_FDCWD,(long)"/proc/self/fd",O_RDONLY|O_DIRECTORY|O_CLOEXEC,0,0,0);if(dir<0)return -1;
    int result=-1;char* bytes=scratch;char* path=scratch+PATH_LIMIT;
    for(;;) {
        long n=raw(__NR_getdents64,dir,(long)bytes,PATH_LIMIT,0,0,0);if(n<=0)break;
        for(long at=0;at<n;) {
            struct CeDent* d=(void*)(bytes+at);if(d->length<20 || d->length>n-at)goto done;
            unsigned fd=0;const char* s=d->name;if(*s>='0'&&*s<='9'){while(*s>='0'&&*s<='9')fd=fd*10+(*s++-'0');
                if(!*s && fd!=(unsigned)dir && ce_fd_path(fd,path)>=0 && prefix(path,pocket_loader_config.root,root_size) && prefix(path+root_size,"/.pocket-ce-locks",17) &&
                   raw(__NR_lseek,fd,0,SEEK_CUR,0,0,0)==original){result=(int)fd;goto done;}}
            at+=d->length;
        }
    }
done:raw(__NR_close,dir,0,0,0,0,0);return result;
}
static void ce_attach_lock(int original,int lock,int flags) {
    raw(__NR_lseek,lock,original,SEEK_SET,0,0,0);raw(__NR_fcntl,lock,F_SETFD,(flags&O_CLOEXEC)?FD_CLOEXEC:0,0,0,0);
}
static long ce_memory_file(int fd,char* bytes) {
    struct stat st;long rc=raw(__NR_fstat,fd,(long)&st,0,0,0,0);if(rc<0)return rc;
    if(st.st_size<0 || st.st_size>1024LL*1024*1024)return -EFBIG;
    long memory=raw(__NR_memfd_create,(long)"pocket-ce-exec",MFD_CLOEXEC|MFD_ALLOW_SEALING,0,0,0,0);if(memory<0)return memory;
    for(long offset=0;offset<st.st_size;) {
        size_t count=st.st_size-offset<4096?(size_t)(st.st_size-offset):4096;
        long n=raw(__NR_pread64,fd,(long)bytes,count,offset,0,0);if(n<=0){rc=n<0?n:-EIO;goto fail;}
        rc=ce_exact(memory,bytes,n,1);if(rc<0)goto fail;offset+=n;
    }
    rc=raw(__NR_fcntl,memory,F_ADD_SEALS,F_SEAL_SEAL|F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE,0,0,0);
    if(rc>=0)return memory;
fail:raw(__NR_close,memory,0,0,0,0,0);return rc;
}
static int ce_dispatch(long nr,__u64* r,ucontext_t* context) {
    if(!ce_enabled)return 0;
    switch(nr) {
    case __NR_fstat:case __NR_ftruncate:case __NR_fchmod:case __NR_fchown:case __NR_getdents64:case __NR_getcwd:
    case __NR_fsync:case __NR_fdatasync:case __NR_fcntl:case __NR_close:case __NR_dup:case __NR_dup3:case __NR_mmap:
    case __NR_openat:case __NR_newfstatat:case __NR_faccessat:case __NR_readlinkat:case __NR_chdir:case __NR_truncate:
    case __NR_mkdirat:case __NR_unlinkat:case __NR_renameat:case __NR_renameat2:case __NR_linkat:case __NR_symlinkat:
    case __NR_fchmodat:case __NR_fchownat:case __NR_utimensat:case __NR_mknodat:case __NR_statfs:
    case __NR_getxattr:case __NR_lgetxattr:case __NR_setxattr:case __NR_lsetxattr:case __NR_listxattr:case __NR_llistxattr:
    case __NR_removexattr:case __NR_lremovexattr:case __NR_execve:case __NR_execveat:case __NR_inotify_add_watch:break;
    case __NR_fstatfs:case __NR_fgetxattr:case __NR_fsetxattr:case __NR_flistxattr:case __NR_fremovexattr:break;
    default:return 0;
    }
    const size_t bytes=sizeof(struct CeReply)+16*PATH_LIMIT;
    long memory=raw(__NR_mmap,0,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(memory<0){r[0]=memory;return 1;}
    struct CeReply* reply=(void*)memory;char* p=(char*)(reply+1),*q=p+PATH_LIMIT,*temp=q+PATH_LIMIT,*scratch=temp+PATH_LIMIT;
    int fd=-1,handled=0;long rc=0;
    if(nr==__NR_newfstatat && (r[3]&AT_EMPTY_PATH)) {
        rc=r[1]?copy_path(temp,r[1]):0;
        if(rc==0) {
            fd=(int)r[0];rc=ce_fd_path(fd,p);
            if(rc>=0 && ce_descriptor(fd,p)) {
                handled=1;rc=ce_fd_request(__NR_fstat,fd,r,reply,p);
                if(!rc)rc=reply->size==sizeof(struct stat)?(ce_copy_out(r[2],reply->data,reply->size)<0?-EFAULT:0):-EIO;
                goto close_reply;
            }
        }
    }
    if(nr==__NR_fstat || nr==__NR_ftruncate || nr==__NR_fchmod || nr==__NR_fchown || nr==__NR_getdents64 || nr==__NR_getcwd || nr==__NR_fsync || nr==__NR_fdatasync || nr==__NR_fcntl || nr==__NR_close || nr==__NR_dup || nr==__NR_dup3 || nr==__NR_mmap || nr==__NR_fstatfs || nr==__NR_fgetxattr || nr==__NR_fsetxattr || nr==__NR_flistxattr || nr==__NR_fremovexattr) {
        if(nr==__NR_getcwd)rc=raw(__NR_getcwd,(long)p,PATH_LIMIT,0,0,0,0);
        else {fd=(int)r[nr==__NR_mmap?4:0];if(nr==__NR_mmap && (r[3]&MAP_ANONYMOUS))goto done;rc=ce_fd_path(fd,p);}
        if(rc<0 || !ce_descriptor(fd,p))goto done;handled=1;
        if(nr==__NR_fstatfs){memcpy(q,pocket_loader_config.root,root_size);memcpy(q+root_size,"/system-data",13);rc=raw(__NR_statfs,(long)q,r[1],0,0,0,0);goto done;}
        if(nr==__NR_fgetxattr || nr==__NR_fsetxattr || nr==__NR_flistxattr || nr==__NR_fremovexattr) {
            q[0]=0;if(nr!=__NR_flistxattr){rc=copy_path(q,r[1]);if(rc<0)goto done;}
            struct stat st;rc=raw(__NR_fstat,fd,(long)&st,0,0,0,0);if(rc<0)goto done;
            struct CeRequest h={.op=2,.nr=(uint32_t)nr,.a={st.st_dev,st.st_ino,r[1],r[2],r[3],r[4]}};
            if(nr==__NR_fsetxattr){if(r[3]>16384){rc=-E2BIG;goto done;}h.data=r[3];rc=ce_copy_in(temp,r[2],h.data);if(rc<0)goto done;}
            rc=ce_rpc(&h,p,q,h.data?temp:0,reply);
            if(rc>=0 && (nr==__NR_fgetxattr || nr==__NR_flistxattr)) {uintptr_t out=r[nr==__NR_flistxattr?1:2];size_t size=r[nr==__NR_flistxattr?2:3];if(size){if(size<reply->size)rc=-ERANGE;else if(ce_copy_out(out,reply->data,reply->size)<0)rc=-EFAULT;}}
            goto close_reply;
        }
        if(nr==__NR_mmap) {
            if(!(r[2]&PROT_EXEC))rc=raw(nr,r[0],r[1],r[2],r[3],r[4],r[5]);
            else if(!(r[3]&MAP_PRIVATE))rc=-EACCES;
            else {long file=ce_memory_file(fd,temp);rc=file<0?file:raw(nr,r[0],r[1],r[2],r[3],file,r[5]);if(file>=0)raw(__NR_close,file,0,0,0,0,0);}goto done;
        }
        if(nr==__NR_fcntl) {
            long command=r[1];int lock=ce_find_lock(fd,scratch);
            if(command==F_GETLK || command==F_SETLK || command==F_SETLKW) {
                if(lock<0){__u64 args[6]={0};rc=ce_fd_request(__NR_fcntl,fd,args,reply,p);if(rc<0)goto done;if(reply->count!=1){rc=-EIO;goto done;}lock=reply->fd[0];ce_attach_lock(fd,lock,raw(__NR_fcntl,fd,F_GETFD,0,0,0,0)&FD_CLOEXEC?O_CLOEXEC:0);}
                struct flock value;rc=copy_from_user(&value,r[2],sizeof(value));if(rc<0)goto done;
                long flags=raw(__NR_fcntl,fd,F_GETFL,0,0,0,0);
                if((value.l_type==F_WRLCK && (flags&O_ACCMODE)==O_RDONLY)||(value.l_type==F_RDLCK && (flags&O_ACCMODE)==O_WRONLY)){rc=-EBADF;goto done;}
                if(value.l_whence==SEEK_CUR || value.l_whence==SEEK_END){struct stat st;long offset=value.l_whence==SEEK_CUR?raw(__NR_lseek,fd,0,SEEK_CUR,0,0,0):raw(__NR_fstat,fd,(long)&st,0,0,0,0);if(offset<0){rc=offset;goto done;}if(value.l_whence==SEEK_END)offset=st.st_size;value.l_start+=offset;value.l_whence=SEEK_SET;}
                rc=raw(__NR_fcntl,lock,command,(long)&value,0,0,0);if(!rc && command==F_GETLK)rc=copy_to_user(r[2],(char*)&value,sizeof(value))<0?-EFAULT:0;goto done;
            }
            rc=raw(nr,r[0],r[1],r[2],r[3],r[4],r[5]);
            if(rc>=0 && command==F_SETFD && lock>=0)raw(__NR_fcntl,lock,F_SETFD,r[2],0,0,0);
            if(rc>=0 && (command==F_DUPFD || command==F_DUPFD_CLOEXEC))goto duplicated;goto done;
        }
        if(nr==__NR_close) {
            int lock=ce_find_lock(fd,scratch);rc=raw(nr,fd,0,0,0,0,0);if(rc==0 && lock>=0)raw(__NR_close,lock,0,0,0,0,0);goto done;
        }
        if(nr==__NR_dup || nr==__NR_dup3) {
            int replaced=nr==__NR_dup3?ce_find_lock((int)r[1],scratch):-1;
            rc=raw(nr,r[0],r[1],r[2],0,0,0);if(rc<0)goto done;if(replaced>=0)raw(__NR_close,replaced,0,0,0,0,0);
duplicated: {long target=rc;__u64 args[6]={0};rc=ce_fd_request(__NR_fcntl,fd,args,reply,p);if(rc<0){raw(__NR_close,target,0,0,0,0,0);goto done;}
                if(reply->count!=1){raw(__NR_close,target,0,0,0,0,0);rc=-EIO;goto done;}
                ce_attach_lock(target,reply->fd[0],raw(__NR_fcntl,target,F_GETFD,0,0,0,0)&FD_CLOEXEC?O_CLOEXEC:0);rc=target;goto done;}
        }
        if(nr==__NR_getcwd) {struct CeRequest h={.op=2,.nr=__NR_getcwd};rc=ce_rpc(&h,p,0,0,reply);if(rc>=0){if(reply->size>r[1])rc=-ERANGE;else rc=copy_to_user(r[0],reply->data,reply->size)<0?-EFAULT:(long)reply->size;}goto close_reply;}
        if(nr==__NR_fsync || nr==__NR_fdatasync) {
            // Flush dirty kernel FUSE pages before committing metadata.
            rc=raw(nr,fd,0,0,0,0,0);if(rc<0)goto done;
        }
        if(nr==__NR_ftruncate && (raw(__NR_fcntl,fd,F_GETFL,0,0,0,0)&O_ACCMODE)==O_RDONLY){rc=-EBADF;goto done;}
        __u64 args[6];for(unsigned i=0;i<6;i++)args[i]=r[i];
        if(nr==__NR_getdents64){args[1]=raw(__NR_lseek,fd,0,SEEK_CUR,0,0,0);args[2]=r[2];}
        rc=ce_fd_request(nr,fd,args,reply,p);
        if(!rc && nr==__NR_fstat)rc=reply->size==sizeof(struct stat)?(copy_to_user(r[1],reply->data,reply->size)<0?-EFAULT:0):-EIO;
        if(!rc && nr==__NR_ftruncate){struct stat st;rc=raw(__NR_fstat,fd,(long)&st,0,0,0,0);}
        if(rc>=0 && nr==__NR_getdents64) {
            if(reply->size>r[2])rc=-EIO;else if(ce_copy_out(r[1],reply->data,reply->size)<0)rc=-EFAULT;
            else if(reply->size){long at=0;int64_t next=0;while(at<(long)reply->size){struct CeDent* d=(void*)(reply->data+at);next=d->offset;at+=d->length;}raw(__NR_lseek,fd,next,SEEK_SET,0,0,0);}
        }
        goto close_reply;
    }
    {unsigned index=nr==__NR_symlinkat?2:(nr==__NR_chdir || nr==__NR_truncate || nr==__NR_statfs || nr==__NR_execve || nr==__NR_getxattr || nr==__NR_lgetxattr || nr==__NR_setxattr || nr==__NR_lsetxattr || nr==__NR_listxattr || nr==__NR_llistxattr || nr==__NR_removexattr || nr==__NR_lremovexattr)?0:1;
        int dir=index==0?AT_FDCWD:(int)r[index-1];rc=ce_path(dir,r[index],p,temp);if(rc<0)goto done;
        if(nr==__NR_renameat || nr==__NR_renameat2 || nr==__NR_linkat) {rc=ce_path((int)r[2],r[3],q,temp);if(rc<0)goto done;}else q[0]=0;
        if(!ce_candidate(p) && !ce_candidate(q))goto done;handled=1;
        if(q[0] && (!ce_candidate(p)||!ce_candidate(q))){rc=-EXDEV;goto done;}
        struct CeRequest h={.op=1,.nr=(uint32_t)nr};for(unsigned i=0;i<6;i++)h.a[i]=r[i];
        if(nr==__NR_openat && (r[2]&O_CREAT))h.a[3]&=~ce_umask(scratch);
        if(nr==__NR_mkdirat)h.a[2]&=~ce_umask(scratch);
        if(nr==__NR_getxattr || nr==__NR_lgetxattr || nr==__NR_setxattr || nr==__NR_lsetxattr || nr==__NR_removexattr || nr==__NR_lremovexattr){rc=copy_path(q,r[1]);if(rc<0)goto done;}
        if(nr==__NR_setxattr || nr==__NR_lsetxattr){if(r[3]>16384){rc=-E2BIG;goto done;}h.data=r[3];rc=ce_copy_in(temp,r[2],h.data);if(rc<0)goto done;}
        if(nr==__NR_symlinkat){rc=copy_path(temp,r[0]);if(rc<0)goto done;h.data=rc;}
        if(nr==__NR_utimensat && r[2]){rc=copy_from_user(temp,r[2],32);if(rc<0)goto done;h.data=32;}
        rc=ce_rpc(&h,p,q,h.data?temp:0,reply);
        if(rc==-EXDEV && reply->size && nr!=__NR_renameat && nr!=__NR_renameat2 && nr!=__NR_linkat) {
            if(reply->size>=PATH_LIMIT){rc=-ENAMETOOLONG;goto close_reply;}reply->data[reply->size]=0;
            if(ce_candidate(reply->data)){rc=-EOPNOTSUPP;goto close_reply;}
            __u64 a[6];for(unsigned i=0;i<6;i++)a[i]=r[i];a[index]=(uintptr_t)reply->data;if(index)a[index-1]=AT_FDCWD;
            path_dispatch(nr,a,context,scratch+4*PATH_LIMIT);rc=a[0];goto close_reply;
        }
        if(rc<0)goto close_reply;
        if(nr==__NR_statfs){memcpy(q,pocket_loader_config.root,root_size);memcpy(q+root_size,"/system-data",13);rc=raw(nr,(long)q,r[1],0,0,0,0);goto close_reply;}
        if(nr==__NR_getxattr || nr==__NR_lgetxattr || nr==__NR_listxattr || nr==__NR_llistxattr) {
            int list=nr==__NR_listxattr || nr==__NR_llistxattr;uintptr_t out=r[list?1:2];size_t size=r[list?2:3];
            if(size){if(size<reply->size)rc=-ERANGE;else if(ce_copy_out(out,reply->data,reply->size)<0)rc=-EFAULT;}goto close_reply;
        }
        if(nr==__NR_openat || nr==__NR_chdir) {
            if(!reply->count){rc=-EIO;goto close_reply;}int opened=reply->fd[0];reply->fd[0]=-1;
            if(nr==__NR_chdir){rc=raw(__NR_fchdir,opened,0,0,0,0,0);raw(__NR_close,opened,0,0,0,0,0);goto close_reply;}
            raw(__NR_fcntl,opened,F_SETFD,(r[2]&O_CLOEXEC)?FD_CLOEXEC:0,0,0,0);raw(__NR_fcntl,opened,F_SETFL,r[2]&(O_APPEND|O_NONBLOCK),0,0,0);
            if(reply->count==2){ce_attach_lock(opened,reply->fd[1],r[2]);reply->fd[1]=-1;}rc=opened;
        } else if(nr==__NR_newfstatat)rc=reply->size==sizeof(struct stat)?(copy_to_user(r[2],reply->data,reply->size)<0?-EFAULT:0):-EIO;
        else if(nr==__NR_readlinkat){size_t count=reply->size<r[3]?reply->size:r[3];rc=copy_to_user(r[2],reply->data,count);}
    }
close_reply:ce_close_reply(reply);
done:if(handled)r[0]=rc;raw(__NR_munmap,memory,bytes,0,0,0,0);return handled;
}
