// SPDX-License-Identifier: GPL-3.0-only
// Freestanding ELF interpreter: no libc, TLS, dynamic dependencies or tracer.
// This supplies compatibility inside the host app sandbox, not isolation.
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <ucontext.h>

#define LOADER_BASE 0x5000000000ULL
#define LOADER_WINDOW 0x200000ULL
#define PATH_LIMIT 4096
#define MAX_PHDR 64
#define FILTER_MARKER 0x504c
#define PROBE_INPUT 0x504c1001UL
#define PROBE_OUTPUT 0x504c1002UL
#define CONTROL_INPUT 0x504c2001UL
#define STATE_LIMIT 640
#define PREINIT_NODE "/sbin/.magisk/device/preinit"

struct PocketLoaderConfig { char magic[8],root[1024],interpreter[1024],linker[1024]; };
__attribute__((section(".pocket_config"),used))
struct PocketLoaderConfig pocket_loader_config={.magic="PCKELF01"};
static size_t root_size;
static size_t page_size;
static int guest_identity,data_alias,module_view,denylist_view;
static unsigned guest_groups[64],guest_group_count;
static unsigned guest_uid[3],guest_gid[3];
// Set when this loader runs as the program itself for an unprepared ELF.
static char exec_target[PATH_LIMIT];
static const char* exec_request;
extern long pocket_raw_syscall(long,long,long,long,long,long,long);
extern long pocket_filter_probe(long);
extern void pocket_restore_rt(void);

// Compiler-generated structure copies must also stay independent of libc/TLS.
void* memcpy(void* d,const void* s,size_t n) {
    unsigned char* a=d;const unsigned char* b=s;for(size_t i=0;i<n;i++)a[i]=b[i];return d;
}
void* memset(void* d,int v,size_t n) {unsigned char* a=d;for(size_t i=0;i<n;i++)a[i]=(unsigned char)v;return d;}
static size_t length(const char* s,size_t limit) {size_t i=0;while(i<limit && s[i])i++;return i;}
static int equal(const char* a,const char* b,size_t n) {for(size_t i=0;i<n;i++)if(a[i]!=b[i])return 0;return 1;}
static long raw(long nr,long a,long b,long c,long d,long e,long f) {return pocket_raw_syscall(nr,a,b,c,d,e,f);}
__attribute__((noreturn)) static void fail(const char* message,long code) {
    raw(__NR_write,2,(long)"POCKET_LOADER_FAIL ",19,0,0,0);
    raw(__NR_write,2,(long)message,length(message,256),0,0,0);
    char number[32];size_t n=sizeof(number);number[--n]='\n';
    unsigned long value=code<0?(unsigned long)-code:(unsigned long)code;
    do {number[--n]='0'+value%10;value/=10;}while(value);
    if(code<0)number[--n]='-';number[--n]=' ';
    raw(__NR_write,2,(long)(number+n),sizeof(number)-n,0,0,0);
    raw(__NR_exit_group,125,0,0,0,0,0);for(;;){}
}
static int prefix(const char* path,const char* p,size_t size) {
    return equal(path,p,size) && (!path[size] || path[size]=='/');
}
static long copy_path(char out[PATH_LIMIT],uintptr_t address) {
    if(address>UINTPTR_MAX-PATH_LIMIT)return -EFAULT;
    int pipes[2];long result=raw(__NR_pipe2,(long)pipes,O_CLOEXEC,0,0,0,0);
    if(result<0)return result;
    size_t used=0;result=-ENAMETOOLONG;
    while(used<PATH_LIMIT) {
        // Do not read beyond a page boundary: a valid terminator can precede
        // an inaccessible next page. Kernel copy_from_user checks readability,
        // including PROT_NONE, without dereferencing application pointers here.
        size_t count=page_size-((address+used)&(page_size-1));
        if(count>PATH_LIMIT-used)count=PATH_LIMIT-used;
        long written=raw(__NR_write,pipes[1],address+used,count,0,0,0);
        if(written<=0) {result=written<0?written:-EFAULT;break;}
        long got=raw(__NR_read,pipes[0],(long)(out+used),written,0,0,0);
        if(got!=written) {result=got<0?got:-EIO;break;}
        for(long i=0;i<got;i++)if(!out[used+(size_t)i]) {result=(long)used+i;goto done;}
        used+=(size_t)got;
        if((size_t)written<count) {result=-EFAULT;break;}
    }
done:raw(__NR_close,pipes[0],0,0,0,0,0);raw(__NR_close,pipes[1],0,0,0,0,0);return result;
}
// read(2) performs a normal checked copy_to_user; unlike /proc/self/mem writes,
// it cannot silently write through a read-only application buffer.
static long copy_to_user(uintptr_t address,const char* bytes,size_t size) {
    int pipes[2];long result=raw(__NR_pipe2,(long)pipes,O_CLOEXEC,0,0,0,0);
    if(result<0)return result;
    result=raw(__NR_write,pipes[1],(long)bytes,size,0,0,0);
    if(result==(long)size)result=raw(__NR_read,pipes[0],address,size,0,0,0);
    else if(result>=0)result=-EIO;
    raw(__NR_close,pipes[0],0,0,0,0,0);raw(__NR_close,pipes[1],0,0,0,0,0);return result;
}
// Latched once published. The Manager and Magisk setup remove a previous
// boot's marker before init, so an incomplete tree never becomes active.
static void module_view_check(void) {
    if(denylist_view){module_view=0;return;}
    if(module_view)return;
    char path[1280];struct stat st;const char marker[]="/.pocket-module-view/ready";
    memcpy(path,pocket_loader_config.root,root_size);memcpy(path+root_size,marker,sizeof(marker));
    module_view=raw(__NR_newfstatat,AT_FDCWD,(long)path,(long)&st,AT_SYMLINK_NOFOLLOW,0,0)==0 && S_ISREG(st.st_mode);
}
static long map_path(const char*,char[PATH_LIMIT]);
static const char* visible_path(char*);
#include "denylist_view.h"
static long map_path(const char* path,char out[PATH_LIMIT]) {
    if(denylist_view && denylist_hidden(path))return -ENOENT;
    if(equal(path,"/dev/hwbinder",14))path="/dev/binder";
    size_t n=length(path,PATH_LIMIT);
    if(n==PATH_LIMIT)return -ENAMETOOLONG;
    if(path[0]!='/' || prefix(path,pocket_loader_config.root,root_size) ||
       prefix(path,"/proc",5) || prefix(path,"/sys",4) ||
       (prefix(path,"/dev",4) && !prefix(path,"/dev/socket",11) &&
        !prefix(path,"/dev/__properties__",19) && !prefix(path,"/dev/input",10) &&
        // Recovery/Magisk installer scratch space is guest-private, not a device.
        !prefix(path,"/dev/tmp",8))) {
        memcpy(out,path,n+1);return 0;
    }
    // Lexical escape rejection only. Symlink containment is not provided.
    for(size_t i=0;i<n;i++)if(path[i]=='/' && path[i+1]=='.' && path[i+2]=='.' &&
        (!path[i+3] || path[i+3]=='/'))return -EPERM;
    if(data_alias && prefix(path,"/data",5)) {
        if(root_size+12+n-5>=PATH_LIMIT)return -ENAMETOOLONG;
        memcpy(out,pocket_loader_config.root,root_size);memcpy(out+root_size,"/system-data",12);
        memcpy(out+root_size+12,path+5,n-5+1);return 0;
    }
    // Magic Mount compatibility: Magisk publishes a fully merged private tree.
    // It supplies enumeration as well as open/stat/exec/dlopen semantics.
    if(module_view && !denylist_view && (prefix(path,"/system",7) || prefix(path,"/vendor",7))) {
        if(root_size+20+n>=PATH_LIMIT)return -ENAMETOOLONG;
        memcpy(out,pocket_loader_config.root,root_size);memcpy(out+root_size,"/.pocket-module-view",20);
        memcpy(out+root_size+20,path,n+1);return 0;
    }
    if(root_size+n>=PATH_LIMIT)return -ENAMETOOLONG;
    memcpy(out,pocket_loader_config.root,root_size);memcpy(out+root_size,path,n+1);return 0;
}
// Bounded kernel-mediated copies: never dereference an application pointer in
// the handler. Small records stay below PIPE_BUF so a pipe cannot deadlock.
static long copy_from_user(void* out,uintptr_t source,size_t size) {
    if(size>PATH_LIMIT || source>UINTPTR_MAX-size)return -EFAULT;
    int pipes[2];long result=raw(__NR_pipe2,(long)pipes,O_CLOEXEC,0,0,0,0);
    if(result<0)return result;
    result=raw(__NR_write,pipes[1],source,size,0,0,0);
    if(result==(long)size)result=raw(__NR_read,pipes[0],(long)out,size,0,0,0);
    else if(result>=0)result=-EFAULT;
    raw(__NR_close,pipes[0],0,0,0,0,0);raw(__NR_close,pipes[1],0,0,0,0,0);
    return result==(long)size?0:result;
}
static const char* visible_path(char* value) {
    if(prefix(value,pocket_loader_config.root,root_size)) {
        if(!value[root_size])return "/";
        if(data_alias && prefix(value+root_size,"/system-data",12)) {
            size_t tail=length(value+root_size+12,PATH_LIMIT-root_size-12);
            // Move downwards in place, retaining the guest /data prefix.
            for(size_t i=0;i<=tail;i++)value[root_size+5+i]=value[root_size+12+i];
            memcpy(value+root_size,"/data",5);
        }
        if(prefix(value+root_size,"/.pocket-module-view",20) &&
           (prefix(value+root_size+20,"/system",7) || prefix(value+root_size+20,"/vendor",7)))
            return value+root_size+20;
        return value+root_size;
    }
    return value;
}
struct Owner {unsigned magic,uid,gid;};
static void owner_path(const struct stat* st,char path[PATH_LIMIT]) {
    memcpy(path,pocket_loader_config.root,root_size);size_t at=root_size;
    const char tail[]="/.pocket-owner/";memcpy(path+at,tail,sizeof(tail)-1);at+=sizeof(tail)-1;
    unsigned long values[2]={(unsigned long)st->st_dev,(unsigned long)st->st_ino};
    for(unsigned i=0;i<2;i++) {for(int j=15;j>=0;j--)path[at++]="0123456789abcdef"[(values[i]>>(j*4))&15];path[at++]='-';}
    path[at-1]=0;
}
static int app_owned(const struct stat* st) {return st->st_uid==(unsigned)raw(__NR_getuid,0,0,0,0,0,0);}
__attribute__((noinline)) static void owner_read(struct stat* st) {
    if(!guest_identity || !app_owned(st))return;
    char path[1280];owner_path(st,path);struct Owner owner;
    long fd=raw(__NR_openat,AT_FDCWD,(long)path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW,0,0,0);
    long size=fd<0?fd:raw(__NR_read,fd,(long)&owner,sizeof(owner),0,0,0);
    if(fd>=0)raw(__NR_close,fd,0,0,0,0,0);
    st->st_uid=size==sizeof(owner) && owner.magic==0x504f434b?owner.uid:0;
    st->st_gid=size==sizeof(owner) && owner.magic==0x504f434b?owner.gid:0;
}
__attribute__((noinline)) static long owner_write(struct stat* st,unsigned uid,unsigned gid) {
    if(!app_owned(st))return -EPERM;
    char path[1280];owner_path(st,path);
    // Record guest ownership without chowning anything on the host. The
    // metadata directory is created by the owning APK before launching init.
    struct Owner owner={0x504f434b,uid,gid};
    long fd=raw(__NR_openat,AT_FDCWD,(long)path,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600,0,0);
    if(fd<0)return fd;
    long rc=raw(__NR_write,fd,(long)&owner,sizeof(owner),0,0,0);raw(__NR_close,fd,0,0,0,0,0);
    return rc==sizeof(owner)?0:rc<0?rc:-EIO;
}
static long owner_change(struct stat* st,unsigned uid,unsigned gid) {
    struct stat visible=*st;owner_read(&visible);
    if(guest_uid[1] && (visible.st_uid!=guest_uid[1] ||
        (uid!=~0U && uid!=visible.st_uid) || (gid!=~0U && gid!=visible.st_gid)))return -EPERM;
    return owner_write(st,uid==~0U?visible.st_uid:uid,gid==~0U?visible.st_gid:gid);
}
// These are guest credentials only. The kernel UID, capabilities, SELinux
// domain and SO_PEERCRED remain the host application's real credentials.
static long identity_call(long nr,__u64* r) {
    if(nr==__NR_getuid)return guest_uid[0];
    if(nr==__NR_geteuid)return guest_uid[1];
    if(nr==__NR_getgid)return guest_gid[0];
    if(nr==__NR_getegid)return guest_gid[1];
    unsigned* ids=(nr==__NR_getresuid || nr==__NR_setuid || nr==__NR_setreuid || nr==__NR_setresuid)?guest_uid:guest_gid;
    if(nr==__NR_getresuid || nr==__NR_getresgid) {
        for(unsigned i=0;i<3;i++) {long rc=copy_to_user(r[i],(const char*)&ids[i],sizeof(ids[i]));if(rc<0)return rc;}
        return 0;
    }
    if(nr==__NR_setuid || nr==__NR_setgid) {
        unsigned requested=(unsigned)r[0];if(requested==~0U)return -EINVAL;
        if(guest_uid[1] && requested!=ids[0] && requested!=ids[2])return -EPERM;
        if(!guest_uid[1])ids[0]=ids[2]=requested;
        ids[1]=requested;return 0;
    }
    unsigned count=(nr==__NR_setreuid || nr==__NR_setregid)?2:3;
    for(unsigned i=0;i<count;i++)if((unsigned)r[i]!=~0U && guest_uid[1]) {
        if(count==2 && i==0) {if((unsigned)r[i]!=ids[0] && (unsigned)r[i]!=ids[1])return -EPERM;}
        else if((unsigned)r[i]!=ids[0] && (unsigned)r[i]!=ids[1] && (unsigned)r[i]!=ids[2])return -EPERM;
    }
    unsigned previous_real=ids[0];
    for(unsigned i=0;i<count;i++)if((unsigned)r[i]!=~0U)ids[i]=(unsigned)r[i];
    if(count==2 && ((unsigned)r[0]!=~0U || ((unsigned)r[1]!=~0U && (unsigned)r[1]!=previous_real)))ids[2]=ids[1];
    return 0;
}
static int identity_number(long nr) {
    return nr==__NR_getuid || nr==__NR_geteuid || nr==__NR_getgid || nr==__NR_getegid ||
        nr==__NR_getresuid || nr==__NR_getresgid || nr==__NR_setuid || nr==__NR_setgid ||
        nr==__NR_setreuid || nr==__NR_setregid || nr==__NR_setresuid || nr==__NR_setresgid;
}
// Private exec transport is consumed by the interpreter before Bionic sees
// environ. It works with an otherwise empty environment and does not need a
// reserved file descriptor that ART could close before exec.
#define STATE_KEY "POCKET_LOADER_STATE_V1="
#define EXEC_KEY "POCKET_LOADER_EXEC_V1="
static char state_hex(unsigned n) {return "0123456789abcdef"[n&15];}
static void identity_encode(char state[STATE_LIMIT]) {
    memcpy(state,STATE_KEY,sizeof(STATE_KEY)-1);size_t at=sizeof(STATE_KEY)-1;
    state[at++]=guest_identity?'1':'0';
    for(unsigned i=0;i<6;i++) {unsigned n=i<3?guest_uid[i]:guest_gid[i-3];
        for(int j=7;j>=0;j--)state[at++]=state_hex(n>>(j*4));}
    state[at++]=state_hex(guest_group_count>>4);state[at++]=state_hex(guest_group_count);
    for(unsigned i=0;i<guest_group_count;i++)for(int j=7;j>=0;j--)state[at++]=state_hex(guest_groups[i]>>(j*4));
    state[at++]=denylist_view?'1':'0';
    state[at]=0;
}
static unsigned hex_value(char c) {return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:16;}
static void identity_decode(const char* state) {
    const size_t key=sizeof(STATE_KEY)-1;
    size_t size=length(state,STATE_LIMIT);
    if(size<key+51 || !equal(state,STATE_KEY,key))return;
    unsigned count=(hex_value(state[key+49])<<4)|hex_value(state[key+50]);
    size_t expected=key+51+count*8;
    if(count>64 || (size!=expected && size!=expected+1))return;
    if(size==expected+1 && state[expected]!='0' && state[expected]!='1')return;
    unsigned ids[6];
    for(unsigned i=0;i<6;i++) {unsigned n=0;for(unsigned j=0;j<8;j++) {
        unsigned d=hex_value(state[key+1+i*8+j]);
        if(d==16)return;n=(n<<4)|d;}ids[i]=n;}
    for(unsigned i=0;i<count;i++) {unsigned n=0;for(unsigned j=0;j<8;j++) {
        unsigned d=hex_value(state[key+51+i*8+j]);if(d==16)return;n=(n<<4)|d;}guest_groups[i]=n;}
    guest_group_count=count;
    guest_identity=state[key]=='1';for(unsigned i=0;i<3;i++) {guest_uid[i]=ids[i];guest_gid[i]=ids[i+3];}
    if(size==expected+1 && state[expected]=='1')denylist_view=1;
}
static void identity_start(uintptr_t* stack,uintptr_t argc) {
    uintptr_t first=argc+2,end=first;
    while(stack[end])end++;
    for(uintptr_t i=first;i<end;) {
        const char* value=(const char*)stack[i];
        int internal=equal(value,STATE_KEY,sizeof(STATE_KEY)-1);
        if(equal(value,"POCKET_LOADER_BOOT=1",20)) {guest_identity=1;internal=1;}
        if(equal(value,EXEC_KEY,sizeof(EXEC_KEY)-1)) {exec_request=value+sizeof(EXEC_KEY)-1;internal=1;}
        if(internal) {
            if(equal(value,STATE_KEY,sizeof(STATE_KEY)-1))identity_decode(value);
            // Remove the private entry and shift the complete auxv too. No
            // argv strings are moved; pointers in auxv retain their meaning.
            uintptr_t last=end+1;while(stack[last]!=AT_NULL)last+=2;last+=2;
            for(uintptr_t j=i;j+1<last;j++)stack[j]=stack[j+1];end--;
        } else i++;
    }
}
static int elf_header(const Elf64_Ehdr* e) {
    return equal((const char*)e->e_ident,ELFMAG,4) && e->e_ident[EI_CLASS]==ELFCLASS64 &&
        e->e_ident[EI_DATA]==ELFDATA2LSB && e->e_machine==EM_AARCH64 &&
        e->e_phentsize==sizeof(Elf64_Phdr) && e->e_phnum>0 && e->e_phnum<=MAX_PHDR &&
        e->e_phoff<1024*1024;
}
// Prepared ELF executables exec directly (0). Others could make the kernel
// select a host interpreter: unprepared AArch64 ELFs, such as static
// binaries an app ships, run through this loader as the program (1).
// Scripts and AT_EMPTY_PATH fail clearly.
static long prepared_exec(int dir,const char* path) {
    long fd=raw(__NR_openat,dir,(long)path,O_RDONLY|O_CLOEXEC,0,0,0);
    if(fd<0)return fd;
    Elf64_Ehdr e;Elf64_Phdr p;char name[1024];long result=-ENOEXEC;
    if(raw(__NR_pread64,fd,(long)&e,sizeof(e),0,0,0)!=sizeof(e) || !elf_header(&e))goto done;
    if(e.e_type==ET_EXEC || e.e_type==ET_DYN)result=1;
    for(unsigned i=0;i<e.e_phnum;i++) {
        if(raw(__NR_pread64,fd,(long)&p,sizeof(p),e.e_phoff+i*sizeof(p),0,0)!=sizeof(p))break;
        if(p.p_type!=PT_INTERP)continue;
        if(p.p_filesz>sizeof(name) || p.p_filesz<2 || p.p_offset>INT64_MAX)break;
        if(raw(__NR_pread64,fd,(long)name,p.p_filesz,p.p_offset,0,0)!=(long)p.p_filesz)break;
        size_t size=length(pocket_loader_config.interpreter,sizeof(name));
        if(size<p.p_filesz && equal(name,pocket_loader_config.interpreter,size+1))result=0;
        // A shared library names no interpreter; a real dynamic one must
        // be Bionic's, which the loader substitutes with the pinned linker.
        else if(p.p_filesz!=sizeof("/system/bin/linker64") || !equal(name,"/system/bin/linker64",p.p_filesz))result=-ENOEXEC;
        break;
    }
done:raw(__NR_close,fd,0,0,0,0,0);return result;
}
struct KernelAction {void (*handler)(int,siginfo_t*,void*);unsigned long flags;void (*restorer)(void);unsigned long mask;};
#include "proc_view.h"
#include "firewall_runtime.h"
#include "binder_runtime.h"
__attribute__((noinline)) static void path_dispatch(long,__u64*,ucontext_t*,char*);
#include "ce_runtime.h"
static void path_call(long,__u64*,ucontext_t*);
static void trapped(int signal,siginfo_t* info,void* opaque) {
    (void)signal;ucontext_t* context=opaque;__u64* r=context->uc_mcontext.regs;
    if(info->si_code!=SYS_SECCOMP || info->si_errno!=FILTER_MARKER || info->si_arch!=AUDIT_ARCH_AARCH64)
        fail("unexpected SIGSYS",info->si_syscall);
    long nr=info->si_syscall;
    if(ce_dispatch(nr,r,context))return;
    if(proc_fd_dispatch(nr,r))return;
    if(nr==__NR_pwrite64 || nr==__NR_pwritev || nr==__NR_pwritev2 || nr==__NR_ftruncate) {
        r[0]=raw(nr,r[0],r[1],r[2],r[3],r[4],r[5]);return;
    }
    if(nr==__NR_mmap) {
        char source[64];struct stat st;
        if(!(r[3]&MAP_ANONYMOUS) && proc_fd_info((int)r[4],source,&st))r[0]=(uint64_t)-ENODEV;
        else r[0]=raw(nr,r[0],r[1],r[2],r[3],r[4],r[5]);
        return;
    }
    if(firewall_dispatch(nr,r))return;
    if(binder_dispatch(nr,r))return;
    if(nr==__NR_getdents64 && denylist_view){r[0]=denylist_getdents((int)r[0],r[1],r[2]);return;}
    if(nr==__NR_ioctl || nr==__NR_close || nr==__NR_dup || nr==__NR_dup3 || nr==__NR_fcntl ||
       nr==__NR_getdents64 || nr==__NR_fchmod || nr==__NR_fsync || nr==__NR_fdatasync || nr==__NR_fstatfs ||
       nr==__NR_fgetxattr || nr==__NR_fsetxattr || nr==__NR_flistxattr || nr==__NR_fremovexattr){
        r[0]=raw(nr,r[0],r[1],r[2],r[3],r[4],r[5]);return;
    }
    if(nr==__NR_getpid) {
        if(r[0]==PROBE_INPUT) {r[0]=PROBE_OUTPUT;return;}
        if(r[0]==CONTROL_INPUT) {
            if(r[1]==4){r[0]=denylist_view;return;}
            if(r[1]==3){
                if(!guest_identity || guest_uid[1]){r[0]=(uint64_t)-EPERM;return;}
                denylist_view=1;module_view=0;r[0]=0;return;
            }
            if(guest_identity && r[1]==2){r[0]=binder_host_scope((unsigned)r[2]);return;}
            if(!guest_identity || r[1]!=1) {r[0]=(uint64_t)-ENOSYS;return;}
            if(r[2]>64) {r[0]=(uint64_t)-EINVAL;return;}
            if(guest_uid[1]) {r[0]=(uint64_t)-EPERM;return;}
            unsigned groups[64];long rc=copy_from_user(groups,r[3],r[2]*sizeof(unsigned));
            if(!rc) {guest_group_count=(unsigned)r[2];memcpy(guest_groups,groups,r[2]*sizeof(unsigned));}
            r[0]=rc;return;
        }
        r[0]=raw(nr,0,0,0,0,0,0);return;
    }
    if(nr==__NR_getgroups && guest_identity) {
        if(!r[0]) {r[0]=guest_group_count;return;}
        if((long)r[0]<0 || r[0]<guest_group_count) {r[0]=(uint64_t)-EINVAL;return;}
        long rc=copy_to_user(r[1],(const char*)guest_groups,guest_group_count*sizeof(unsigned));
        r[0]=rc<0?rc:guest_group_count;return;
    }
    if(nr==__NR_getgroups) {r[0]=raw(nr,r[0],r[1],0,0,0,0);return;}
    if(nr==__NR_settimeofday) {r[0]=(uint64_t)-EPERM;return;}
    if(nr==__NR_timerfd_create) {
        long clock=r[0];if(clock==8)clock=0;if(clock==9)clock=7;
        r[0]=raw(nr,clock,r[1],0,0,0,0);return;
    }
    if(nr==__NR_rt_sigprocmask) {
        if(r[3]!=8) {r[0]=(uint64_t)-EINVAL;return;}
        unsigned long mask,previous=*(unsigned long*)&context->uc_sigmask;
        if(r[1]) {
            long rc=copy_from_user(&mask,r[1],sizeof(mask));if(rc<0) {r[0]=rc;return;}
            if(r[0]!=SIG_BLOCK && r[0]!=SIG_UNBLOCK && r[0]!=SIG_SETMASK) {r[0]=(uint64_t)-EINVAL;return;}
        }
        if(r[2]) {long rc=copy_to_user(r[2],(const char*)&previous,sizeof(previous));if(rc<0) {r[0]=rc;return;}}
        if(r[1]) {
            unsigned long next=r[0]==SIG_BLOCK?previous|mask:r[0]==SIG_UNBLOCK?previous&~mask:mask;
            next&=~(1UL<<(SIGSYS-1));*(unsigned long*)&context->uc_sigmask=next;
        }
        r[0]=0;return;
    }
    if(nr==__NR_rt_sigaction) {
        if(r[0]==SIGSYS && r[1]) {r[0]=(uint64_t)-EBUSY;return;}
        struct KernelAction action;long address=r[1];
        if(address) {
            long rc=copy_from_user(&action,address,sizeof(action));if(rc<0) {r[0]=rc;return;}
            action.mask&=~(1UL<<(SIGSYS-1));
            address=(long)&action;
        }
        r[0]=raw(nr,r[0],address,r[2],r[3],r[4],r[5]);return;
    }
    if(identity_number(nr)) {
        r[0]=guest_identity?(uint64_t)identity_call(nr,r):(uint64_t)raw(nr,r[0],r[1],r[2],r[3],r[4],r[5]);
        if(guest_identity && !r[0] && (nr==__NR_setuid || nr==__NR_setresuid || nr==__NR_setreuid))binder_publish_identity();return;
    }
    if(nr==__NR_getcwd) {
        long memory=raw(__NR_mmap,0,PATH_LIMIT,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(memory<0) {r[0]=memory;return;}
        char* cwd=(char*)memory;long rc=raw(nr,memory,PATH_LIMIT,0,0,0,0);
        if(rc>0) {const char* value=visible_path(cwd);size_t size=length(value,PATH_LIMIT)+1;
            rc=size>r[1]?-ERANGE:copy_to_user(r[0],value,size);}
        raw(__NR_munmap,memory,PATH_LIMIT,0,0,0,0);r[0]=rc;return;
    }
    if(nr==__NR_bind || nr==__NR_connect) {
        if(nr==__NR_connect) {
            long rc=firewall_address((int)r[0],r[1],r[2]);
            if(rc<0) {r[0]=rc;return;}
        }
        // Abstract UNIX addresses and IP sockets retain their kernel ABI.
        struct sockaddr_un address;long rc;
        if(r[2]>sizeof(address) || r[2]<sizeof(address.sun_family)) {
            r[0]=raw(nr,r[0],r[1],r[2],0,0,0);return;
        }
        memset(&address,0,sizeof(address));rc=copy_from_user(&address,r[1],r[2]);
        if(rc<0) {r[0]=rc;return;}
        if(address.sun_family==AF_UNIX && r[2]>offsetof(struct sockaddr_un,sun_path) && address.sun_path[0]) {
            size_t size=r[2]-offsetof(struct sockaddr_un,sun_path);
            if(length(address.sun_path,size)==size) {r[0]=(uint64_t)-ENAMETOOLONG;return;}
            long memory=raw(__NR_mmap,0,PATH_LIMIT,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            if(memory<0) {r[0]=memory;return;}
            char* mapped=(char*)memory;rc=map_path(address.sun_path,mapped);
            size_t n=rc<0?0:length(mapped,PATH_LIMIT);
            if(!rc && n>=sizeof(address.sun_path))rc=-ENAMETOOLONG;
            if(!rc) {memcpy(address.sun_path,mapped,n+1);
                rc=raw(nr,r[0],(long)&address,offsetof(struct sockaddr_un,sun_path)+n+1,0,0,0);}
            raw(__NR_munmap,memory,PATH_LIMIT,0,0,0,0);r[0]=rc;return;
        }
        r[0]=raw(nr,r[0],r[1],r[2],0,0,0);return;
    }
    if(nr==__NR_getsockname || nr==__NR_getpeername) {
        struct sockaddr_un address;unsigned capacity=0,size=sizeof(address);
        long rc=copy_from_user(&capacity,r[2],sizeof(capacity));
        if(rc<0) {r[0]=rc;return;}
        rc=raw(nr,r[0],(long)&address,(long)&size,0,0,0);
        if(!rc && address.sun_family==AF_UNIX && size<=sizeof(address) && address.sun_path[0]) {
            address.sun_path[sizeof(address.sun_path)-1]=0;
            const char* visible=visible_path(address.sun_path);
            size_t n=length(visible,sizeof(address.sun_path));
            for(size_t i=0;i<=n;i++)address.sun_path[i]=visible[i];
            size=offsetof(struct sockaddr_un,sun_path)+n+1;
        }
        if(!rc) {unsigned count=capacity<size?capacity:size;
            if(count>sizeof(address))count=sizeof(address);
            rc=copy_to_user(r[1],(const char*)&address,count);
            if(rc>=0)rc=copy_to_user(r[2],(const char*)&size,sizeof(size));if(rc>=0)rc=0;}
        r[0]=rc;return;
    }
    if(nr==__NR_fchown && !guest_identity) {r[0]=raw(nr,r[0],r[1],r[2],0,0,0);return;}
    if(nr==__NR_fchown && guest_identity) {
        struct stat st;long rc=raw(__NR_fstat,r[0],(long)&st,0,0,0,0);
        r[0]=rc<0?rc:owner_change(&st,r[1],r[2]);return;
    }
    if(nr==__NR_fstat) {
        struct stat st;long rc=raw(nr,r[0],(long)&st,0,0,0,0);
        if(!rc) {proc_stat_view((int)r[0],&st);owner_read(&st);}
        r[0]=rc<0?rc:copy_to_user(r[1],(const char*)&st,sizeof(st))<0?-EFAULT:0;return;
    }
    path_call(nr,r,context);
}
__attribute__((noinline)) static void path_dispatch(long nr,__u64* r,ucontext_t* context,char* scratch);
__attribute__((noinline)) static void path_call(long nr,__u64* r,ucontext_t* context) {
    // A trap can occur inside ART's handler on a small alternate stack.
    // Keep handler frames small; large, private buffers never use that stack.
    const size_t size=8*PATH_LIMIT;
    long memory=raw(__NR_mmap,0,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(memory<0) {r[0]=memory;return;}
    path_dispatch(nr,r,context,(char*)memory);
    raw(__NR_munmap,memory,size,0,0,0,0);
}
__attribute__((noinline)) static void path_dispatch(long nr,__u64* r,ucontext_t* context,char* scratch) {
    unsigned argument=nr==__NR_symlinkat?2:(nr==__NR_execve || nr==__NR_chdir || nr==__NR_truncate || nr==__NR_statfs ||
        nr==__NR_getxattr || nr==__NR_lgetxattr || nr==__NR_setxattr || nr==__NR_lsetxattr ||
        nr==__NR_listxattr || nr==__NR_llistxattr || nr==__NR_removexattr || nr==__NR_lremovexattr)?0:1;
    char* original=scratch;char* mapped=scratch+PATH_LIMIT;
    // Long-lived init execs services after Magisk publishes the view.
    if(nr==__NR_execve || nr==__NR_execveat)module_view_check();
    long status=copy_path(original,r[argument]);
    if(status>=0)status=denylist_view?denylist_at(argument?(int)r[argument-1]:AT_FDCWD,original,mapped,scratch+3*PATH_LIMIT):map_path(original,mapped);
    if(status<0) {r[0]=status;return;}
    if(denylist_view && denylist_resolved(argument?(int)r[argument-1]:AT_FDCWD,mapped,scratch+3*PATH_LIMIT)){r[0]=(uint64_t)-ENOENT;return;}
#ifdef POCKET_LOADER_DIAGNOSTICS
    raw(__NR_write,2,(long)"LOADER_PATH ",12,0,0,0);
    raw(__NR_write,2,(long)original,length(original,PATH_LIMIT),0,0,0);
    raw(__NR_write,2,(long)" -> ",4,0,0,0);
    raw(__NR_write,2,(long)mapped,length(mapped,PATH_LIMIT),0,0,0);
    raw(__NR_write,2,(long)"\n",1,0,0,0);
#endif
    long a[6];for(unsigned i=0;i<6;i++)a[i]=r[i];a[argument]=(long)mapped;
    char* second=scratch+2*PATH_LIMIT;
    if(nr==__NR_renameat || nr==__NR_renameat2 || nr==__NR_linkat) {
        char* path=scratch+3*PATH_LIMIT;status=copy_path(path,r[3]);if(status>=0)status=denylist_view?denylist_at((int)r[2],path,second,scratch+5*PATH_LIMIT):map_path(path,second);
        if(status<0) {r[0]=status;return;}a[3]=(long)second;
    }
    if(nr==__NR_symlinkat) {
        // Relative targets remain relative; absolute targets point into this
        // instance. readlink reverses the physical prefix for guest callers.
        char* target=scratch+3*PATH_LIMIT;status=copy_path(target,r[0]);
        if(status>=0)status=map_path(target,second);
        if(status<0) {r[0]=status;return;}
        a[0]=(long)second;
    }
    if(nr==__NR_fchownat && guest_identity) {
        if(a[4] & ~(AT_SYMLINK_NOFOLLOW|AT_EMPTY_PATH)) {r[0]=(uint64_t)-EINVAL;return;}
        struct stat st;status=raw(__NR_newfstatat,a[0],a[1],(long)&st,a[4],0,0);
        r[0]=status<0?status:owner_change(&st,r[2],r[3]);return;
    }
    if(nr==__NR_newfstatat) {
        struct stat st;status=raw(nr,a[0],a[1],(long)&st,a[3],0,0);
        if(!status && !(a[3]&AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) && !st.st_nlink) {
            if(!mapped[0] && (a[3]&AT_EMPTY_PATH))proc_stat_view((int)a[0],&st);
            else {
                long fd=raw(__NR_openat,a[0],a[1],O_PATH|O_CLOEXEC,0,0,0);
                if(fd>=0) {proc_stat_view((int)fd,&st);raw(__NR_close,fd,0,0,0,0,0);}
            }
        }
        // Magisk's preinit partition: setup.sh backs it with a persistent
        // directory and leaves this regular file as its node, since the app
        // cannot create block devices. The node reports its backing filesystem.
        if(!status && S_ISREG(st.st_mode) && equal(original,PREINIT_NODE,sizeof(PREINIT_NODE))) {
            st.st_mode=S_IFBLK|(st.st_mode&07777);st.st_rdev=st.st_dev;st.st_size=0;
        }
        if(!status)owner_read(&st);
        r[0]=status<0?status:copy_to_user(r[2],(const char*)&st,sizeof(st))<0?-EFAULT:0;return;
    }
    if(nr==__NR_readlinkat) {
        if(!r[3] || r[3]>INT32_MAX) {r[0]=(uint64_t)-EINVAL;return;}
        char* value=scratch+3*PATH_LIMIT;status=raw(nr,a[0],a[1],(long)value,PATH_LIMIT,0,0);
        if(status<0) {r[0]=status;return;}
        const char* visible=value;size_t size=(size_t)status;
        char source[64];struct stat original_stat;
        if(size<PATH_LIMIT) {
            value[size]=0;
            if(proc_decode_link(value,source,&original_stat)) {size=length(source,sizeof(source));memcpy(value,source,size+1);}
            // A loader-run program sees its own executable, not this loader.
            else if(exec_target[0] && equal(value,pocket_loader_config.interpreter,size+1)) {
                size=length(exec_target,PATH_LIMIT);memcpy(value,exec_target,size+1);
            }
        }
        if(size<PATH_LIMIT) {value[size]=0;visible=visible_path(value);size=length(visible,PATH_LIMIT);}
        if(size>r[3])size=r[3];
        r[0]=copy_to_user(r[2],visible,size);return;
    }
    if(nr==__NR_execve || nr==__NR_execveat) {
        if(nr==__NR_execveat && a[4]) {r[0]=(uint64_t)-EOPNOTSUPP;return;}
        // Busybox and Magisk re-exec themselves by this name.
        if(exec_target[0] && equal(mapped,"/proc/self/exe",15) && (nr==__NR_execve || a[0]==AT_FDCWD))
            memcpy(mapped,exec_target,length(exec_target,PATH_LIMIT)+1);
        status=prepared_exec(nr==__NR_execve?AT_FDCWD:(int)a[0],mapped);
        if(status<0) {r[0]=status;return;}
        char* request=scratch+6*PATH_LIMIT;
        if(status) {
            // The kernel loads this loader as a static program; it maps the
            // target with the same handler and identity transport.
            size_t key=sizeof(EXEC_KEY)-1,n=length(mapped,PATH_LIMIT),at=key;
            if(mapped[0]!='/') {
                if(nr==__NR_execveat && a[0]!=AT_FDCWD) {r[0]=(uint64_t)-ENOEXEC;return;}
                status=raw(__NR_getcwd,(long)request+key,PATH_LIMIT-key,0,0,0,0);
                if(status<0) {r[0]=status;return;}
                at=key+length(request+key,PATH_LIMIT-key);request[at++]='/';
            }
            if(at+n>=PATH_LIMIT) {r[0]=(uint64_t)-ENAMETOOLONG;return;}
            memcpy(request,EXEC_KEY,key);memcpy(request+at,mapped,n+1);
            a[nr==__NR_execve?0:1]=(long)pocket_loader_config.interpreter;
        } else request=0;
        // Exec preserves signal masks. Restore the pre-trap mask before losing
        // this signal frame, so SIGSYS remains usable in the next interpreter.
        char* state=scratch+7*PATH_LIMIT;identity_encode(state);
        uintptr_t* env=(uintptr_t*)(scratch+4*PATH_LIMIT);unsigned count=0;uintptr_t source=nr==__NR_execve?r[2]:r[3];
        if(source)for(;count<1022;count++) {
            status=copy_from_user(env+count,source+count*sizeof(uintptr_t),sizeof(uintptr_t));
            if(status<0) {r[0]=status;return;}if(!env[count])break;
        }
        if(count==1022) {r[0]=(uint64_t)-E2BIG;return;}
        env[count++]=(uintptr_t)state;if(request)env[count++]=(uintptr_t)request;env[count]=0;a[nr==__NR_execve?2:3]=(long)env;
        status=raw(__NR_rt_sigprocmask,SIG_SETMASK,(long)&context->uc_sigmask,0,8,0,0);
        if(status<0) {r[0]=status;return;}
        r[0]=raw(nr,a[0],a[1],a[2],a[3],a[4],a[5]);return;
    }
    int created=0;struct stat old;
    if(guest_identity && (nr==__NR_mkdirat || (nr==__NR_openat && (a[2]&O_CREAT))))
        created=raw(__NR_newfstatat,a[0],a[1],(long)&old,AT_SYMLINK_NOFOLLOW,0,0)==-ENOENT;
    long result=raw(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
    if(nr==__NR_openat && result<0)result=proc_reopen_at((int)a[0],mapped,(int)a[2],result);
    if(nr==__NR_openat && result>=0)result=proc_open_view((int)result,(int)a[2]);
    if(result>=0 && created) {
        struct stat st;long rc=nr==__NR_openat?raw(__NR_fstat,result,(long)&st,0,0,0,0):
            raw(__NR_newfstatat,a[0],a[1],(long)&st,AT_SYMLINK_NOFOLLOW,0,0);
        if(!rc)rc=owner_write(&st,guest_uid[1],guest_gid[1]);
        if(rc<0) {if(nr==__NR_openat)raw(__NR_close,result,0,0,0,0,0);result=rc;}
    }
    r[0]=result;
}
static void install_handler(void) {
    struct KernelAction action={trapped,SA_SIGINFO|0x04000000UL,pocket_restore_rt,0};
    long result=raw(__NR_rt_sigaction,SIGSYS,(long)&action,0,8,0,0);
    if(result<0)fail("sigaction",result);
}
static void install_filter(void) {
    struct sock_filter code[320];unsigned used=0;
#define STMT(c,k) do {code[used++]=(struct sock_filter)BPF_STMT(c,k);}while(0)
#define JUMP(c,k,t,f) do {code[used++]=(struct sock_filter)BPF_JUMP(c,k,t,f);}while(0)
    STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch));
    JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_AARCH64,1,0);
    STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL);
    STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,instruction_pointer)+4);
    JUMP(BPF_JMP|BPF_JEQ|BPF_K,LOADER_BASE>>32,0,3);
    STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,instruction_pointer));
    JUMP(BPF_JMP|BPF_JGE|BPF_K,LOADER_WINDOW,1,0);
    STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW);
    STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr));
    // An opt-in getpid argument distinguishes our filter from host filters.
    JUMP(BPF_JMP|BPF_JEQ|BPF_K,__NR_getpid,0,5);
    STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args));
    JUMP(BPF_JMP|BPF_JEQ|BPF_K,PROBE_INPUT,2,0);
    JUMP(BPF_JMP|BPF_JEQ|BPF_K,CONTROL_INPUT,1,0);
    STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW);
    STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP|FILTER_MARKER);
    const unsigned numbers[]={__NR_mmap,__NR_ioctl,__NR_close,__NR_dup,__NR_dup3,__NR_fcntl,__NR_openat,__NR_newfstatat,__NR_faccessat,__NR_readlinkat,
        __NR_getdents64,__NR_fchmod,__NR_fsync,__NR_fdatasync,
        __NR_fstatfs,__NR_fgetxattr,__NR_fsetxattr,__NR_flistxattr,__NR_fremovexattr,
        __NR_sendto,__NR_sendmsg,__NR_sendmmsg,__NR_write,__NR_writev,__NR_pwrite64,__NR_pwritev,__NR_pwritev2,__NR_ftruncate,__NR_sendfile,__NR_splice,__NR_copy_file_range,
        __NR_execve,__NR_execveat,__NR_rt_sigaction,__NR_rt_sigprocmask,__NR_getcwd,__NR_chdir,
        __NR_mkdirat,__NR_unlinkat,__NR_renameat,__NR_renameat2,__NR_linkat,__NR_symlinkat,
        __NR_fchmodat,__NR_fchownat,__NR_utimensat,__NR_truncate,__NR_mknodat,
        __NR_statfs,__NR_inotify_add_watch,__NR_getxattr,__NR_lgetxattr,__NR_setxattr,__NR_lsetxattr,
        __NR_listxattr,__NR_llistxattr,__NR_removexattr,__NR_lremovexattr,__NR_getgroups,
        __NR_timerfd_create,__NR_settimeofday,__NR_bind,__NR_connect,__NR_getsockname,__NR_getpeername,__NR_fstat,__NR_fchown,
        __NR_getuid,__NR_geteuid,__NR_getgid,__NR_getegid,__NR_getresuid,__NR_getresgid,
        __NR_setuid,__NR_setgid,__NR_setreuid,__NR_setregid,__NR_setresuid,__NR_setresgid};
    for(unsigned i=0;i<sizeof(numbers)/sizeof(numbers[0]);i++) {
        JUMP(BPF_JMP|BPF_JEQ|BPF_K,numbers[i],0,1);
        STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP|FILTER_MARKER);
    }
    STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW);
    struct sock_fprog program={(unsigned short)used,code};
    long result=raw(__NR_prctl,PR_SET_NO_NEW_PRIVS,1,0,0,0,0);
    if(!result)result=raw(__NR_prctl,PR_SET_SECCOMP,SECCOMP_MODE_FILTER,(long)&program,0,0,0);
    if(result<0)fail("seccomp",result);
#undef STMT
#undef JUMP
}
static uintptr_t align_down(uintptr_t n,size_t page) {return n&~(page-1);}
static uintptr_t align_up(uintptr_t n,size_t page) {return (n+page-1)&~(page-1);}
// Maps the pinned linker, or for a loader-run program its executable. An
// ET_EXEC program keeps its link addresses; *program describes it on return.
struct Program {uintptr_t phdr,phnum,entry;int interpreter;};
static uintptr_t load_elf(const char* path,size_t page,uintptr_t* bias,struct Program* program) {
    long fd=raw(__NR_openat,AT_FDCWD,(long)path,O_RDONLY|O_CLOEXEC,0,0,0);
    if(fd<0)fail("open linker",fd);
    Elf64_Ehdr e;Elf64_Phdr p[MAX_PHDR];
    if(raw(__NR_pread64,fd,(long)&e,sizeof(e),0,0,0)!=sizeof(e) || !elf_header(&e) ||
       !(e.e_type==ET_DYN || (program && e.e_type==ET_EXEC)))
        fail("linker ELF",-ENOEXEC);
    size_t bytes=e.e_phnum*sizeof(p[0]);
    if(raw(__NR_pread64,fd,(long)p,bytes,e.e_phoff,0,0)!=(long)bytes)fail("linker phdr",-ENOEXEC);
    uintptr_t low=UINTPTR_MAX,high=0;
    for(unsigned i=0;i<e.e_phnum;i++)if(p[i].p_type==PT_LOAD) {
        if(p[i].p_vaddr>64*1024*1024 || p[i].p_memsz>64*1024*1024 || p[i].p_filesz>p[i].p_memsz ||
           p[i].p_offset>INT64_MAX || (p[i].p_vaddr%page)!=(p[i].p_offset%page))fail("linker segment",-ENOEXEC);
        uintptr_t start=align_down(p[i].p_vaddr,page),end=align_up(p[i].p_vaddr+p[i].p_memsz,page);
        if(start<low)low=start;if(end>high)high=end;
    }
    if(low==UINTPTR_MAX || high<=low || high-low>64*1024*1024)fail("linker span",-ENOEXEC);
    int fixed=e.e_type==ET_EXEC;
    long reserved=raw(__NR_mmap,fixed?low:0,high-low,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|(fixed?MAP_FIXED_NOREPLACE:0),-1,0);
    if(reserved<0 || (fixed && (uintptr_t)reserved!=low))fail("reserve linker",reserved<0?reserved:-EEXIST);
    *bias=(uintptr_t)reserved-low;
    if(program) {
        program->phdr=0;program->phnum=e.e_phnum;program->entry=*bias+e.e_entry;program->interpreter=0;
        for(unsigned i=0;i<e.e_phnum;i++) {
            if(p[i].p_type==PT_INTERP)program->interpreter=1;
            if(p[i].p_type==PT_PHDR)program->phdr=*bias+p[i].p_vaddr;
        }
        for(unsigned i=0;!program->phdr && i<e.e_phnum;i++)
            if(p[i].p_type==PT_LOAD && p[i].p_offset<=e.e_phoff && e.e_phoff+bytes<=p[i].p_offset+p[i].p_filesz)
                program->phdr=*bias+p[i].p_vaddr+(e.e_phoff-p[i].p_offset);
        if(!program->phdr)fail("program phdr",-ENOEXEC);
    }
    for(unsigned i=0;i<e.e_phnum;i++)if(p[i].p_type==PT_LOAD && p[i].p_memsz) {
        uintptr_t start=align_down(p[i].p_vaddr,page),file_end=align_up(p[i].p_vaddr+p[i].p_filesz,page);
        uintptr_t end=align_up(p[i].p_vaddr+p[i].p_memsz,page);
        int prot=((p[i].p_flags&PF_R)?PROT_READ:0)|((p[i].p_flags&PF_W)?PROT_WRITE:0)|((p[i].p_flags&PF_X)?PROT_EXEC:0);
        long mapped;
        if(p[i].p_filesz) {
            mapped=raw(__NR_mmap,*bias+start,file_end-start,prot,MAP_PRIVATE|MAP_FIXED,fd,align_down(p[i].p_offset,page));
            if(mapped<0)fail("map linker segment",mapped);
        } else file_end=start;
        if(end>file_end) {
            mapped=raw(__NR_mmap,*bias+file_end,end-file_end,prot,MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS,-1,0);
            if(mapped<0)fail("map linker BSS",mapped);
        }
        if(p[i].p_memsz>p[i].p_filesz && p[i].p_filesz && file_end>p[i].p_vaddr+p[i].p_filesz) {
            uintptr_t from=*bias+p[i].p_vaddr+p[i].p_filesz;
            size_t count=file_end-p[i].p_vaddr-p[i].p_filesz;
            if(!(prot&PROT_WRITE))fail("read-only linker BSS tail",-ENOEXEC);
            memset((void*)from,0,count);
        }
    }
    raw(__NR_close,fd,0,0,0,0,0);return *bias+e.e_entry;
}
uintptr_t pocket_loader_start(uintptr_t* stack) {
    root_size=length(pocket_loader_config.root,sizeof(pocket_loader_config.root));
    if(!root_size || root_size==sizeof(pocket_loader_config.root) || pocket_loader_config.root[0]!='/')fail("instance config",-EINVAL);
    uintptr_t argc=*stack;
    if(argc>65536)fail("argc",-E2BIG);
    identity_start(stack,argc);
    uintptr_t at=argc+2;
    if(argc>65536)fail("argc",-E2BIG);
    while(stack[at])if(++at>131072)fail("environment",-E2BIG);
    Elf64_auxv_t* aux=(Elf64_auxv_t*)(stack+at+1);size_t page=0;
    Elf64_auxv_t* base=NULL;
    for(unsigned i=0;i<128 && aux[i].a_type!=AT_NULL;i++) {
        if(aux[i].a_type==AT_PAGESZ)page=aux[i].a_un.a_val;
        if(aux[i].a_type==AT_BASE)base=aux+i;
    }
    if(!base || (page!=4096 && page!=16384))fail("auxv",-ENOEXEC);
    page_size=page;
    char data_path[1280];memcpy(data_path,pocket_loader_config.root,root_size);
    memcpy(data_path+root_size,"/system-data",13);struct stat data_stat;
    data_alias=raw(__NR_newfstatat,AT_FDCWD,(long)data_path,(long)&data_stat,0,0,0)==0 && S_ISDIR(data_stat.st_mode);
    memcpy(data_path+root_size,"/.pocket-ce-enabled",sizeof("/.pocket-ce-enabled"));
    ce_enabled=raw(__NR_newfstatat,AT_FDCWD,(long)data_path,(long)&data_stat,0,0,0)==0;
    if(ce_enabled) {
        memcpy(data_path+root_size,"/.pocket-ce-device",sizeof("/.pocket-ce-device"));
        long config=raw(__NR_openat,AT_FDCWD,(long)data_path,O_RDONLY|O_CLOEXEC,0,0,0);
        if(config>=0){if(raw(__NR_read,config,(long)&ce_fuse_device,sizeof(ce_fuse_device),0,0,0)!=(long)sizeof(ce_fuse_device))ce_fuse_device=0;raw(__NR_close,config,0,0,0,0,0);}
    }
    module_view_check();
    binder_publish_identity();
    install_handler();
    if(pocket_filter_probe(PROBE_INPUT)!=PROBE_OUTPUT)install_filter();
    if(pocket_filter_probe(PROBE_INPUT)!=PROBE_OUTPUT)fail("filter probe",-ENOSYS);
    if(exec_request) {
        size_t n=length(exec_request,PATH_LIMIT);
        if(n==PATH_LIMIT || exec_request[0]!='/')fail("exec target",-ENAMETOOLONG);
        memcpy(exec_target,exec_request,n+1);
        struct Program program;uintptr_t bias;load_elf(exec_target,page,&bias,&program);
        for(unsigned i=0;i<128 && aux[i].a_type!=AT_NULL;i++) {
            if(aux[i].a_type==AT_PHDR)aux[i].a_un.a_val=program.phdr;
            if(aux[i].a_type==AT_PHNUM)aux[i].a_un.a_val=program.phnum;
            if(aux[i].a_type==AT_ENTRY)aux[i].a_un.a_val=program.entry;
        }
        if(!program.interpreter) {base->a_un.a_val=0;return program.entry;}
    }
    uintptr_t bias,entry=load_elf(pocket_loader_config.linker,page,&bias,NULL);base->a_un.a_val=bias;
    return entry;
}
