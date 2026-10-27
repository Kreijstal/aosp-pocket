// SPDX-License-Identifier: GPL-3.0-only
// Guest path names in proc mapping reports. Records and kernel measurements
// are retained; this is a compatibility view, not a concealment boundary.
#pragma once
#include <linux/memfd.h>

#define PROC_VIEW_TAG "pocket-proc-v1:"
#define PROC_VIEW_LINE 32768
#define PROC_VIEW_LIMIT (16*1024*1024)
#define PROC_VIEW_SEALS (F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)
#define PROC_VIEW_STAT_CHARS ((sizeof(struct stat)*8+5)/6)

static size_t proc_number(char* out,unsigned n) {
    char digits[16];size_t used=0;do {digits[used++]='0'+n%10;n/=10;}while(n);
    size_t size=used;while(used)*out++=digits[--used];return size;
}
static void proc_fd_path(int fd,char out[64]) {
    const char base[]="/proc/self/fd/";memcpy(out,base,sizeof(base)-1);
    out[sizeof(base)-1+proc_number(out+sizeof(base)-1,(unsigned)fd)]=0;
}
// Resolve through the kernel first, so numeric PIDs, thread-self, dirfds and
// symlink aliases all select the same implementation.
static int proc_mapping_file(const char* path) {
    if(!equal(path,"/proc/",6))return 0;
    const char* p=path+6;if(*p<'0' || *p>'9')return 0;
    while(*p>='0' && *p<='9')p++;
    if(equal(p,"/task/",6)) {
        p+=6;if(*p<'0' || *p>'9')return 0;
        while(*p>='0' && *p<='9')p++;
    }
    if(equal(p,"/maps",6) || equal(p,"/smaps",7))return 1;
    if(equal(p,"/fdinfo/",8)) {
        p+=8;if(*p<'0' || *p>'9')return 0;
        while(*p>='0' && *p<='9')p++;
        if(!*p)return 2;
    }
    return 0;
}
// Metadata travels with the sealed file rather than an fd-number table.
// dup, fork, fd reuse and non-CLOEXEC exec therefore need no shadow fd state.
static const char proc_alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static void proc_encode_stat(char* out,const struct stat* st) {
    const unsigned char* bytes=(const unsigned char*)st;
    for(size_t i=0;i<PROC_VIEW_STAT_CHARS;i++) {
        size_t bit=i*6,byte=bit/8;unsigned pair=(unsigned)bytes[byte]<<8;
        if(byte+1<sizeof(*st))pair|=bytes[byte+1];
        out[i]=proc_alphabet[(pair>>(10-(bit%8)))&63];
    }
}
static int proc_decode_link(const char* link,char source[64],struct stat* st) {
    const char tag[]="/memfd:" PROC_VIEW_TAG;
    size_t size=length(link,384),head=sizeof(tag)-1;
    if(size<head+PROC_VIEW_STAT_CHARS+1+sizeof(" (deleted)")-1 || !equal(link,tag,head))return 0;
    const char* p=link+head;unsigned value=0,bits=0;size_t at=0;
    unsigned char* bytes=(unsigned char*)st;
    for(size_t i=0;i<PROC_VIEW_STAT_CHARS;i++) {
        unsigned digit=0;while(digit<64 && proc_alphabet[digit]!=p[i])digit++;
        if(digit==64)return 0;value=(value<<6)|digit;bits+=6;
        if(bits>=8) {bits-=8;if(at<sizeof(*st))bytes[at++]=(unsigned char)(value>>bits);}
    }
    p+=PROC_VIEW_STAT_CHARS;if(*p++!=':')return 0;
    size_t count=size-(size_t)(p-link)-(sizeof(" (deleted)")-1);
    if(!count || count>=64 || !equal(p+count," (deleted)",sizeof(" (deleted)")))return 0;
    memcpy(source,p,count);source[count]=0;
    return at==sizeof(*st) && proc_mapping_file(source);
}
static int proc_fd_info(int fd,char source[64],struct stat* st) {
    if(fd<0)return 0;
    struct stat actual;
    if(raw(__NR_fstat,fd,(long)&actual,0,0,0,0)<0 || !S_ISREG(actual.st_mode) || actual.st_nlink)return 0;
    char path[64],link[384];proc_fd_path(fd,path);
    long size=raw(__NR_readlinkat,AT_FDCWD,(long)path,(long)link,sizeof(link)-1,0,0);
    if(size<=0 || size>=(long)sizeof(link)-1)return 0;link[size]=0;
    if(!proc_decode_link(link,source,st))return 0;
    long seals=raw(__NR_fcntl,fd,F_GET_SEALS,0,0,0,0);
    // Android allows O_PATH to the file but may deny reopening app tmpfs
    // files for reading. O_PATH cannot query seals; the tag supplies metadata
    // only, and is deliberately not treated as a security credential.
    if(seals==-EBADF) {
        long flags=raw(__NR_fcntl,fd,F_GETFL,0,0,0,0);
        if(flags>=0 && (flags&O_PATH))return 1;
    }
    return seals>=0 && (seals&PROC_VIEW_SEALS)==PROC_VIEW_SEALS;
}
static size_t proc_translate_mapping(char* line,size_t size) {
    // Only change the pathname field of a maps/smaps header. Addresses,
    // permissions, offsets, device/inode and all smaps statistics stay intact.
    size_t at=0;int dash=0;
    while(at<size && line[at]!=' ' && line[at]!='\t') {
        char c=line[at++];if(c=='-') {if(dash)return size;dash=1;}
        else if(!((c>='0'&&c<='9') || (c>='a'&&c<='f') || (c>='A'&&c<='F')))return size;
    }
    if(!dash)return size;
    for(unsigned field=1;field<5;field++) {
        while(at<size && (line[at]==' ' || line[at]=='\t'))at++;
        while(at<size && line[at]!=' ' && line[at]!='\t' && line[at]!='\n')at++;
    }
    while(at<size && (line[at]==' ' || line[at]=='\t'))at++;
    if(size-at<root_size || !equal(line+at,pocket_loader_config.root,root_size))return size;
    size_t from=at+root_size;
    if(from<size && line[from]!='/' && line[from]!='\n')return size;
    if(size-from>=27 && equal(line+from,"/.pocket-module-view",20) &&
       (equal(line+from+20,"/system",7)||equal(line+from+20,"/vendor",7)) &&
       (from+27==size || line[from+27]=='/' || line[from+27]=='\n'))from+=20;
    if(data_alias && size-from>=12 && equal(line+from,"/system-data",12) &&
       (from+12==size || line[from+12]=='/' || line[from+12]=='\n')) {
        memcpy(line+at,"/data",5);at+=5;from+=12;
    }
    if(from==size || line[from]=='\n')line[at++]='/';
    while(from<size)line[at++]=line[from++];return at;
}
static long proc_write_all(int fd,const char* bytes,size_t size) {
    size_t at=0;while(at<size) {
        long n=raw(__NR_write,fd,(long)(bytes+at),size-at,0,0,0);
        if(n==-EINTR)continue;if(n<=0)return n<0?n:-EIO;at+=(size_t)n;
    }return 0;
}
static size_t proc_unsigned(char* out,unsigned long n,unsigned base) {
    char digits[32];size_t used=0;do {digits[used++]='0'+n%base;n/=base;}while(n);
    size_t count=used;while(used)*out++=digits[--used];return count;
}
static long proc_mount_id(const char* source) {
    long fd=raw(__NR_openat,AT_FDCWD,(long)source,O_RDONLY|O_CLOEXEC,0,0,0);
    if(fd<0)fd=raw(__NR_openat,AT_FDCWD,(long)"/proc/self/maps",O_RDONLY|O_CLOEXEC,0,0,0);
    if(fd<0)return fd;
    char path[64]="/proc/self/fdinfo/",data[512];
    path[18+proc_number(path+18,(unsigned)fd)]=0;
    long info=raw(__NR_openat,AT_FDCWD,(long)path,O_RDONLY|O_CLOEXEC,0,0,0);
    long count=info<0?info:raw(__NR_read,info,(long)data,sizeof(data),0,0,0);
    raw(__NR_close,fd,0,0,0,0,0);if(info>=0)raw(__NR_close,info,0,0,0,0,0);
    if(count<0)return count;
    for(long i=0;i+7<count;i++)if((!i || data[i-1]=='\n') && equal(data+i,"mnt_id:",7)) {
        i+=7;while(i<count && (data[i]==' ' || data[i]=='\t'))i++;
        unsigned long value=0;unsigned digits=0;
        while(i<count && data[i]>='0' && data[i]<='9') {
            if(value>INT32_MAX/10)return -EOVERFLOW;value=value*10+(unsigned)(data[i++]-'0');digits++;
        }
        return digits && value<=INT32_MAX?(long)value:-EIO;
    }
    return -EIO;
}
static size_t proc_translate_fdinfo(char* line,size_t size,const struct stat* target,long mount_id) {
    size_t at=0;unsigned base=10;unsigned long value;
    if(size>=4 && equal(line,"ino:",4)) {at=4;value=target->st_ino;}
    else if(size>=7 && equal(line,"mnt_id:",7)) {at=7;value=(unsigned long)mount_id;}
    else if(size>=6 && equal(line,"flags:",6)) {
        at=6;while(at<size && (line[at]==' ' || line[at]=='\t'))at++;
        value=0;for(size_t i=at;i<size && line[i]>='0' && line[i]<='7';i++)value=(value<<3)|(unsigned)(line[i]-'0');
        value&=~O_ACCMODE;base=8;
    }else return size;
    while(at<size && (line[at]==' ' || line[at]=='\t'))at++;
    at+=proc_unsigned(line+at,value,base);line[at++]='\n';return at;
}
static long proc_open_view(int fd,int flags) {
    if(flags&O_PATH)return fd;
    char fdpath[64],source[64];proc_fd_path(fd,fdpath);
    long count=raw(__NR_readlinkat,AT_FDCWD,(long)fdpath,(long)source,sizeof(source)-1,0,0);
    if(count<=0 || count>=(long)sizeof(source)-1)return fd;source[count]=0;
    int kind=proc_mapping_file(source);if(!kind)return fd;
    struct stat target_stat;long mount_id=0;
    if(kind==2) {
        char linkpath[64],link[384],original[64];size_t split=6;
        while(source[split] && !equal(source+split,"/fdinfo/",8))split++;
        if(!source[split])return fd;
        memcpy(linkpath,source,split);memcpy(linkpath+split,"/fd/",4);
        size_t tail=length(source+split+8,64-split-8);memcpy(linkpath+split+4,source+split+8,tail+1);
        long n=raw(__NR_readlinkat,AT_FDCWD,(long)linkpath,(long)link,sizeof(link)-1,0,0);
        if(n<=0 || n>=(long)sizeof(link)-1)return fd;link[n]=0;
        if(!proc_decode_link(link,original,&target_stat))return fd;
        mount_id=proc_mount_id(original);
        if(mount_id<0) {raw(__NR_close,fd,0,0,0,0,0);return mount_id;}
    }
    // Do not translate a different host process into this guest instance.
    char exe[64],target[PATH_LIMIT];size_t at=6;
    while(source[at]>='0' && source[at]<='9')at++;
    memcpy(exe,source,at);memcpy(exe+at,"/exe",5);
    count=raw(__NR_readlinkat,AT_FDCWD,(long)exe,(long)target,sizeof(target)-1,0,0);
    if(count<=0 || count>=(long)sizeof(target)-1)return fd;target[count]=0;
    if(!prefix(target,pocket_loader_config.root,root_size))return fd;
    long result=-EACCES,output=-1,memory=-1;
#ifdef POCKET_LOADER_DIAGNOSTICS
    char stage='a';
#define PROC_STAGE(c) (stage=(c))
#else
#define PROC_STAGE(c) ((void)0)
#endif
    if((flags&O_ACCMODE)!=O_RDONLY)goto done;
    PROC_STAGE('s');
    struct stat st;result=raw(__NR_fstat,fd,(long)&st,0,0,0,0);if(result<0)goto done;
    char name[250];size_t head=sizeof(PROC_VIEW_TAG)-1,n=length(source,sizeof(source));
    if(head+PROC_VIEW_STAT_CHARS+1+n>=sizeof(name)) {result=-ENAMETOOLONG;goto done;}
    memcpy(name,PROC_VIEW_TAG,head);proc_encode_stat(name+head,&st);head+=PROC_VIEW_STAT_CHARS;
    name[head++]=':';memcpy(name+head,source,n+1);
    PROC_STAGE('c');output=raw(__NR_memfd_create,(long)name,MFD_CLOEXEC|MFD_ALLOW_SEALING,0,0,0,0);
    if(output<0) {result=output;goto done;}
    PROC_STAGE('m');memory=raw(__NR_mmap,0,PROC_VIEW_LINE+8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(memory<0) {result=memory;goto done;}
    char* line=(char*)memory;char* input=line+PROC_VIEW_LINE;size_t used=0,total=0;
    PROC_STAGE('r');for(;;) {
        count=raw(__NR_read,fd,(long)input,8192,0,0,0);
        if(count==-EINTR)continue;if(count<0) {result=count;goto done;}
        if(!count)break;
        if((size_t)count>PROC_VIEW_LIMIT-total) {result=-EFBIG;goto done;}total+=(size_t)count;
        for(long i=0;i<count;i++) {
            if(used==PROC_VIEW_LINE) {result=-ENAMETOOLONG;goto done;}
            line[used++]=input[i];
            if(input[i]=='\n') {
                size_t n=kind==1?proc_translate_mapping(line,used):proc_translate_fdinfo(line,used,&target_stat,mount_id);
                result=proc_write_all(output,line,n);used=0;if(result<0)goto done;
            }
        }
    }
    if(used) {
        size_t n=kind==1?proc_translate_mapping(line,used):proc_translate_fdinfo(line,used,&target_stat,mount_id);
        result=proc_write_all(output,line,n);if(result<0)goto done;
    }
    PROC_STAGE('l');result=raw(__NR_fcntl,output,F_ADD_SEALS,PROC_VIEW_SEALS,0,0,0);if(result<0)goto done;
    // Android can deny reopening an app's memfd through /proc/self/fd. Keep
    // the sealed description and supply its read-only access semantics in the
    // syscall layer. Kernel seals also reject writes outside that layer.
    PROC_STAGE('o');result=raw(__NR_lseek,output,0,SEEK_SET,0,0,0);if(result<0)goto done;
    result=raw(__NR_fcntl,output,F_SETFL,flags&O_NONBLOCK,0,0,0);if(result<0)goto done;
    result=raw(__NR_fcntl,output,F_SETFD,(flags&O_CLOEXEC)?FD_CLOEXEC:0,0,0,0);if(result<0)goto done;
    result=output;output=-1;
done:
#ifdef POCKET_LOADER_DIAGNOSTICS
    if(result<0) {
        char message[64]="PROC_VIEW_ERROR stage=";size_t n=sizeof("PROC_VIEW_ERROR stage=")-1;
        message[n++]=stage;message[n++]=' ';n+=proc_number(message+n,(unsigned)-result);message[n++]='\n';
        raw(__NR_write,2,(long)message,n,0,0,0);
    }
#endif
#undef PROC_STAGE
    raw(__NR_close,fd,0,0,0,0,0);
    if(output>=0)raw(__NR_close,output,0,0,0,0,0);
    if(memory>=0)raw(__NR_munmap,memory,PROC_VIEW_LINE+8192,0,0,0,0);
    return result;
}
static long proc_reopen_at(int dir,const char* path,int flags,long failure) {
    if(failure!=-EACCES || (flags&O_ACCMODE)!=O_RDONLY || (flags&O_NOFOLLOW))return failure;
    long fd=raw(__NR_openat,dir,(long)path,O_PATH|O_CLOEXEC,0,0,0);if(fd<0)return failure;
    char source[64];struct stat st;int snapshot=proc_fd_info((int)fd,source,&st);raw(__NR_close,fd,0,0,0,0,0);
    if(!snapshot)return failure;
    fd=raw(__NR_openat,AT_FDCWD,(long)source,flags,0,0,0);
    return fd<0?fd:proc_open_view((int)fd,flags);
}
static int proc_fd_dispatch(long nr,__u64* r) {
    int argument=0;
    if(nr==__NR_fcntl) {if(r[1]!=F_GETFL)return 0;}
    else if(nr==__NR_splice || nr==__NR_copy_file_range)argument=2;
    else if(nr!=__NR_write && nr!=__NR_writev && nr!=__NR_pwrite64 && nr!=__NR_pwritev &&
            nr!=__NR_pwritev2 && nr!=__NR_ftruncate && nr!=__NR_sendfile)return 0;
    char source[64];struct stat st;
    if(!proc_fd_info((int)r[argument],source,&st))return 0;
    if(nr==__NR_fcntl) {long flags=raw(nr,r[0],F_GETFL,0,0,0,0);r[0]=flags<0?flags:(flags&~O_ACCMODE)|O_RDONLY;}
    else r[0]=(uint64_t)-EBADF;
    return 1;
}
static void proc_stat_view(int fd,struct stat* st) {
    // Ordinary file fstats avoid the extra proc readlink entirely.
    if(!S_ISREG(st->st_mode) || st->st_nlink)return;
    char source[64];struct stat original;
    if(proc_fd_info(fd,source,&original))*st=original;
}
