// SPDX-License-Identifier: GPL-3.0-only
// Opt-in filesystem semantics for immutable Android platform partitions.
// The backing tree remains host-owned and updatable outside the loader. This
// compatibility view is not a kernel isolation boundary (as with the loader).
#pragma once
static int readonly_physical(const char* path) {
    if(!prefix(path,pocket_loader_config.root,root_size))return 0;
    const char* p=path+root_size;
    if(equal(p,"/.pocket-readonly-system",sizeof("/.pocket-readonly-system")))return 1;
    if(prefix(p,"/.pocket-module-view",20))p+=20;
    const char* partitions[]={"/system","/system_ext","/vendor","/product","/odm","/oem","/apex"};
    for(unsigned i=0;i<sizeof(partitions)/sizeof(*partitions);i++)
        if(prefix(p,partitions[i],length(partitions[i],32)))return 1;
    return 0;
}
static int readonly_fd(int fd,char* scratch) {
    return denylist_fd_path(fd,scratch)>=0 && readonly_physical(scratch);
}
static long readonly_target(int dir,const char* path,int nofollow,int empty,char* scratch) {
    if(!path[0] && empty)return readonly_fd(dir,scratch)?-EROFS:0;
    long fd=raw(__NR_openat,dir,(long)path,O_PATH|O_CLOEXEC|(nofollow?O_NOFOLLOW:0),0,0,0);
    // Preserve the operation's own ENOENT, EACCES, ELOOP, etc.
    if(fd<0)return 0;
    int blocked=readonly_fd((int)fd,scratch);raw(__NR_close,fd,0,0,0,0,0);
    return blocked?-EROFS:0;
}
static long readonly_entry(int dir,const char* path,char* scratch) {
    long rc=readonly_target(dir,path,1,0,scratch);if(rc<0)return rc;
    size_t n=length(path,PATH_LIMIT);if(!n || n==PATH_LIMIT)return 0;
    // Resolve the parent through the kernel, including relative paths, dirfds,
    // symlinks and proc magic links. Never lexically collapse symlink/.. pairs.
    char* parent=scratch+PATH_LIMIT;memcpy(parent,path,n+1);
    while(n>1 && parent[n-1]=='/')parent[--n]=0;
    while(n && parent[n-1]!='/')n--;
    if(!n){parent[0]='.';parent[1]=0;}else if(n==1)parent[1]=0;else parent[n-1]=0;
    return readonly_target(dir,parent,0,0,scratch);
}
static long readonly_creation(int dir,const char* path,char* scratch) {
    // A dangling symlink may have a writable parent but resolve into /system.
    // Walk final symlinks using their kernel-resolved parents, not lexical '..'.
    char* current=scratch+3*PATH_LIMIT;char* target=scratch+2*PATH_LIMIT;
    size_t n=length(path,PATH_LIMIT);if(n==PATH_LIMIT)return 0;memcpy(current,path,n+1);
    for(unsigned links=0;links<40;links++) {
        long fd=raw(__NR_openat,dir,(long)current,O_PATH|O_CLOEXEC|O_NOFOLLOW,0,0,0);
        if(fd<0)return fd==-ENOENT?readonly_entry(dir,current,scratch):0;
        struct stat st;long rc=raw(__NR_fstat,fd,(long)&st,0,0,0,0);
        if(rc<0 || !S_ISLNK(st.st_mode)){raw(__NR_close,fd,0,0,0,0,0);return 0;}
        rc=denylist_fd_path((int)fd,scratch);
        long count=raw(__NR_readlinkat,fd,(long)"",(long)target,PATH_LIMIT-1,0,0);
        raw(__NR_close,fd,0,0,0,0,0);if(rc<0 || count<0)return 0;target[count]=0;
        size_t head=0;
        if(target[0]!='/'){head=length(scratch,PATH_LIMIT);while(head && scratch[head-1]!='/')head--;}
        if(head+(size_t)count>=PATH_LIMIT)return -ENAMETOOLONG;
        memcpy(current,scratch,head);memcpy(current+head,target,(size_t)count+1);
    }
    return 0; // The real syscall supplies ELOOP.
}
static long readonly_path_check(long nr,long* a,char* scratch) {
    int dir=(int)a[0];const char* path=(const char*)a[1];
    if(nr==__NR_faccessat && (a[2]&2 /* W_OK */)) {
        long rc=raw(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
        return rc<0?rc:readonly_target(dir,path,0,0,scratch);
    }
    if(nr==__NR_openat) {
        long flags=a[2];
        if((flags&O_ACCMODE)!=O_RDONLY || (flags&O_TRUNC) || (flags&O_TMPFILE)==O_TMPFILE) {
            long rc=readonly_target(dir,path,flags&O_NOFOLLOW,0,scratch);if(rc<0)return rc;
        }
        if(flags&O_CREAT) {
            struct stat st;
            if(raw(__NR_newfstatat,dir,(long)path,(long)&st,0,0,0)==-ENOENT)
                return readonly_creation(dir,path,scratch);
        }
    }
    if(nr==__NR_mkdirat || nr==__NR_mknodat || nr==__NR_unlinkat)
        return readonly_entry(dir,path,scratch);
    if(nr==__NR_symlinkat)return readonly_entry((int)a[1],(const char*)a[2],scratch);
    if(nr==__NR_renameat || nr==__NR_renameat2 || nr==__NR_linkat) {
        long rc=nr==__NR_linkat?readonly_target(dir,path,!(a[4]&AT_SYMLINK_FOLLOW),a[4]&AT_EMPTY_PATH,scratch):readonly_entry(dir,path,scratch);
        return rc<0?rc:readonly_entry((int)a[2],(const char*)a[3],scratch);
    }
    if(nr==__NR_fchmodat || nr==__NR_fchownat || nr==__NR_utimensat) {
        int flags=nr==__NR_fchownat?a[4]:nr==__NR_utimensat?a[3]:0;
        // utimensat(fd, NULL, ...) acts on the descriptor.
        if(nr==__NR_utimensat && !a[1])return readonly_fd(dir,scratch)?-EROFS:0;
        return readonly_target(dir,path,flags&AT_SYMLINK_NOFOLLOW,flags&AT_EMPTY_PATH,scratch);
    }
    if(nr==__NR_truncate || nr==__NR_setxattr || nr==__NR_lsetxattr || nr==__NR_removexattr || nr==__NR_lremovexattr)
        return readonly_target(AT_FDCWD,(const char*)a[0],nr==__NR_lsetxattr || nr==__NR_lremovexattr,0,scratch);
    return 0;
}
static int readonly_fd_dispatch(long nr,__u64* r) {
    if(!readonly_view || !(nr==__NR_ftruncate || nr==__NR_fchmod || nr==__NR_fchown ||
        nr==__NR_fsetxattr || nr==__NR_fremovexattr))return 0;
    long memory=raw(__NR_mmap,0,PATH_LIMIT,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(memory<0){r[0]=memory;return 1;}
    int blocked=readonly_fd((int)r[0],(char*)memory);
    raw(__NR_munmap,memory,PATH_LIMIT,0,0,0,0);
    if(blocked)r[0]=(uint64_t)-EROFS;return blocked;
}
