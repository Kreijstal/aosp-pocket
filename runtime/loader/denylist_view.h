// SPDX-License-Identifier: GPL-3.0-only
// Process-local guest view. This does not change host mounts or isolate UIDs.
#pragma once
static long copy_to_user(uintptr_t,const char*,size_t);
static int denylist_hidden(const char* path) {
    const char* p=path;
    if(prefix(p,pocket_loader_config.root,root_size))p+=root_size;
    if(prefix(p,"/data/adb",9) || prefix(p,"/system-data/adb",16) || prefix(p,"/.pocket-module-view",20))return 1;
    const char* dirs[]={"/system/bin/","/system/xbin/","/sbin/"};
    const char* names[]={"su","magisk",".magisk","resetprop","magiskpolicy","magiskinit","magiskboot"};
    for(unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++) {
        size_t n=length(dirs[i],32);if(!equal(p,dirs[i],n))continue;
        for(unsigned j=0;j<sizeof(names)/sizeof(*names);j++)if(prefix(p+n,names[j],length(names[j],32)))return 1;
    }
    return 0;
}
static long denylist_normalize(const char* in,char* out) {
    size_t n=0,at=0;if(in[0]!='/')return -EINVAL;out[n++]='/';
    while(in[at]) {
        while(in[at]=='/')at++;size_t first=at;while(in[at] && in[at]!='/')at++;size_t count=at-first;
        if(!count || (count==1 && in[first]=='.'))continue;
        if(count==2 && in[first]=='.' && in[first+1]=='.') {
            if(n>1){while(n>1 && out[n-1]!='/')n--;if(n>1)n--;}continue;
        }
        if(n>1)out[n++]='/';if(n+count>=PATH_LIMIT)return -ENAMETOOLONG;
        memcpy(out+n,in+first,count);n+=count;
    }
    out[n]=0;return (long)n;
}
static long denylist_fd_path(int fd,char* out) {
    char link[64]="/proc/self/fd/",digits[16];unsigned n=0,value=(unsigned)fd;
    do{digits[n++]='0'+value%10;value/=10;}while(value);
    unsigned at=14;while(n)link[at++]=digits[--n];link[at]=0;
    long rc=raw(__NR_readlinkat,AT_FDCWD,(long)link,(long)out,PATH_LIMIT-1,0,0);if(rc>=0)out[rc]=0;return rc;
}
static long denylist_at(int dir,const char* path,char* mapped,char* temporary) {
    if(path[0]=='/') {
        long rc=denylist_normalize(path,temporary);if(rc<0)return rc;
        // Magic links to guest directories must use the same base view as
        // openat(dirfd, ...). Resolve only the prefix, retaining the suffix.
        if(equal(temporary,"/proc/",6)) {
            size_t at=6;while(temporary[at] && temporary[at]!='/')at++;
            if(equal(temporary+at,"/fd/",4)) {
                at+=4;size_t first=at;while(temporary[at]>='0'&&temporary[at]<='9')at++;
                if(at==first || (temporary[at] && temporary[at]!='/'))return map_path(temporary,mapped);
            }else if((equal(temporary+at,"/cwd",4)||equal(temporary+at,"/root",5))) {
                at+=temporary[at+1]=='c'?4:5;if(temporary[at] && temporary[at]!='/')return map_path(temporary,mapped);
            }else return map_path(temporary,mapped);
            char saved=temporary[at];
            // The final proc link itself must retain readlink/lstat semantics.
            if(!saved)return map_path(temporary,mapped);
            temporary[at]=0;
            long n=raw(__NR_readlinkat,AT_FDCWD,(long)temporary,(long)mapped,PATH_LIMIT-1,0,0);temporary[at]=saved;
            if(n>=0) {
                mapped[n]=0;
                if(prefix(mapped,pocket_loader_config.root,root_size)) {
                    const char* visible=visible_path(mapped);size_t head=length(visible,PATH_LIMIT),tail=length(temporary+at,PATH_LIMIT);
                    if(head+tail+1>PATH_LIMIT)return -ENAMETOOLONG;
                    // The destination is below the source in the same buffer.
                    memcpy(mapped,visible,head);memcpy(mapped+head,temporary+at,tail+1);
                    rc=denylist_normalize(mapped,temporary);if(rc<0)return rc;
                }
            }
        }
        return map_path(temporary,mapped);
    }
    if(!path[0]){mapped[0]=0;return 0;}
    long rc=dir==AT_FDCWD?raw(__NR_getcwd,(long)temporary,PATH_LIMIT,0,0,0,0):denylist_fd_path(dir,temporary);
    if(rc<0 || !prefix(temporary,pocket_loader_config.root,root_size))return map_path(path,mapped);
    const char* visible=visible_path(temporary);size_t a=length(visible,PATH_LIMIT),b=length(path,PATH_LIMIT);
    if(a+b+2>PATH_LIMIT)return -ENAMETOOLONG;
    memcpy(mapped,visible,a);mapped[a++]='/';memcpy(mapped+a,path,b+1);
    rc=denylist_normalize(mapped,temporary);return rc<0?rc:map_path(temporary,mapped);
}
static int denylist_resolved(int dir,const char* path,char* scratch) {
    long fd=raw(__NR_openat,dir,(long)path,O_PATH|O_CLOEXEC,0,0,0);if(fd<0)return 0;
    long rc=denylist_fd_path((int)fd,scratch);raw(__NR_close,fd,0,0,0,0,0);
    return rc>=0 && denylist_hidden(scratch);
}
struct DenyDent {uint64_t ino;int64_t offset;unsigned short size;unsigned char type;char name[];};
static long denylist_getdents(int fd,uintptr_t out,size_t size) {
    if(!size)return raw(__NR_getdents64,fd,out,size,0,0,0);
    const size_t allocation=5*PATH_LIMIT;long memory=raw(__NR_mmap,0,allocation,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(memory<0)return memory;
    char* path=(char*)memory;char* mapped=path+PATH_LIMIT;char* child=mapped+PATH_LIMIT;char* bytes=child+PATH_LIMIT;
    int actual=fd,owned=-1;long result=denylist_fd_path(fd,path);
    if(result<0)goto done;
    {const char* p=visible_path(path);size_t n=length(p,PATH_LIMIT);memcpy(path,p,n+1);}
    if(denylist_hidden(path)){result=-ENOENT;goto done;}
    // An inherited overlay directory fd gets the base directory's entries.
    result=map_path(path,mapped);if(result<0)goto done;
    if(path[0]=='/' && (prefix(path,"/system",7)||prefix(path,"/vendor",7))) {
        owned=raw(__NR_openat,AT_FDCWD,(long)mapped,O_RDONLY|O_DIRECTORY|O_CLOEXEC,0,0,0);if(owned<0){result=owned;goto done;}
        long position=raw(__NR_lseek,fd,0,SEEK_CUR,0,0,0);if(position<0){result=position;goto done;}
        result=raw(__NR_lseek,owned,position,SEEK_SET,0,0,0);if(result<0)goto done;actual=owned;
    }
    for(;;) {
        size_t count=size>PATH_LIMIT?PATH_LIMIT:size;result=raw(__NR_getdents64,actual,(long)bytes,count,0,0,0);
        if(owned>=0){long position=raw(__NR_lseek,owned,0,SEEK_CUR,0,0,0);if(position>=0)raw(__NR_lseek,fd,position,SEEK_SET,0,0,0);}
        if(result<=0)break;size_t used=0;
        for(long at=0;at<result;) {
            struct DenyDent* entry=(void*)(bytes+at);if(entry->size<20 || entry->size>result-at){result=-EIO;goto done;}
            size_t n=length(path,PATH_LIMIT),tail=length(entry->name,entry->size-19);
            if(tail==entry->size-19 || n+tail+2>PATH_LIMIT){result=-EIO;goto done;}
            memcpy(child,path,n);if(n>1)child[n++]='/';memcpy(child+n,entry->name,tail+1);
            if(!denylist_hidden(child)){memcpy(bytes+used,entry,entry->size);used+=entry->size;}
            at+=entry->size;
        }
        if(used){result=copy_to_user(out,bytes,used)<0?-EFAULT:(long)used;break;}
    }
done:if(owned>=0)raw(__NR_close,owned,0,0,0,0,0);raw(__NR_munmap,memory,allocation,0,0,0,0);return result;
}
