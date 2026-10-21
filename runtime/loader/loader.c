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
#include <sys/syscall.h>
#include <ucontext.h>

#define LOADER_BASE 0x5000000000ULL
#define LOADER_WINDOW 0x200000ULL
#define PATH_LIMIT 4096
#define MAX_PHDR 64
#define FILTER_MARKER 0x504c
#define PROBE_INPUT 0x504c1001UL
#define PROBE_OUTPUT 0x504c1002UL

struct PocketLoaderConfig { char magic[8],root[1024],interpreter[1024],linker[1024]; };
__attribute__((section(".pocket_config"),used))
struct PocketLoaderConfig pocket_loader_config={.magic="PCKELF01"};
static size_t root_size;
static size_t page_size;
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
static long map_path(const char* path,char out[PATH_LIMIT]) {
    size_t n=length(path,PATH_LIMIT);
    if(n==PATH_LIMIT)return -ENAMETOOLONG;
    if(path[0]!='/' || prefix(path,pocket_loader_config.root,root_size) ||
       prefix(path,"/proc",5) || prefix(path,"/sys",4) || prefix(path,"/dev",4)) {
        memcpy(out,path,n+1);return 0;
    }
    // Lexical escape rejection only. Symlink containment is not provided.
    for(size_t i=0;i<n;i++)if(path[i]=='/' && path[i+1]=='.' && path[i+2]=='.' &&
        (!path[i+3] || path[i+3]=='/'))return -EPERM;
    if(root_size+n>=PATH_LIMIT)return -ENAMETOOLONG;
    memcpy(out,pocket_loader_config.root,root_size);memcpy(out+root_size,path,n+1);return 0;
}
static int elf_header(const Elf64_Ehdr* e) {
    return equal((const char*)e->e_ident,ELFMAG,4) && e->e_ident[EI_CLASS]==ELFCLASS64 &&
        e->e_ident[EI_DATA]==ELFDATA2LSB && e->e_machine==EM_AARCH64 &&
        e->e_phentsize==sizeof(Elf64_Phdr) && e->e_phnum>0 && e->e_phnum<=MAX_PHDR &&
        e->e_phoff<1024*1024;
}
// Only prepared ELF executables may exec. Otherwise the kernel could select a
// host interpreter. Scripts, AT_EMPTY_PATH and unprepared binaries fail clearly.
static long prepared_exec(int dir,const char* path) {
    long fd=raw(__NR_openat,dir,(long)path,O_RDONLY|O_CLOEXEC,0,0,0);
    if(fd<0)return fd;
    Elf64_Ehdr e;Elf64_Phdr p;char name[1024];long result=-ENOEXEC;
    if(raw(__NR_pread64,fd,(long)&e,sizeof(e),0,0,0)!=sizeof(e) || !elf_header(&e))goto done;
    for(unsigned i=0;i<e.e_phnum;i++) {
        if(raw(__NR_pread64,fd,(long)&p,sizeof(p),e.e_phoff+i*sizeof(p),0,0)!=sizeof(p))break;
        if(p.p_type!=PT_INTERP)continue;
        if(p.p_filesz>sizeof(name) || p.p_filesz<2 || p.p_offset>INT64_MAX)break;
        if(raw(__NR_pread64,fd,(long)name,p.p_filesz,p.p_offset,0,0)!=(long)p.p_filesz)break;
        size_t size=length(pocket_loader_config.interpreter,sizeof(name));
        if(size<p.p_filesz && equal(name,pocket_loader_config.interpreter,size+1))result=0;
        break;
    }
done:raw(__NR_close,fd,0,0,0,0,0);return result;
}
static void trapped(int signal,siginfo_t* info,void* opaque) {
    (void)signal;ucontext_t* context=opaque;__u64* r=context->uc_mcontext.regs;
    if(info->si_code!=SYS_SECCOMP || info->si_errno!=FILTER_MARKER || info->si_arch!=AUDIT_ARCH_AARCH64)
        fail("unexpected SIGSYS",info->si_syscall);
    long nr=info->si_syscall;
    if(nr==__NR_getpid) {r[0]=r[0]==PROBE_INPUT?PROBE_OUTPUT:(unsigned long)raw(nr,0,0,0,0,0,0);return;}
    if(nr==__NR_rt_sigaction) {
        r[0]=r[0]==SIGSYS && r[1] ? (uint64_t)-EBUSY :
            (uint64_t)raw(nr,r[0],r[1],r[2],r[3],r[4],r[5]);return;
    }
    unsigned argument=nr==__NR_execve?0:1;
    char original[PATH_LIMIT],mapped[PATH_LIMIT];
    long status=copy_path(original,r[argument]);
    if(status>=0)status=map_path(original,mapped);
    if(status<0) {r[0]=status;return;}
#ifdef POCKET_LOADER_DIAGNOSTICS
    raw(__NR_write,2,(long)"LOADER_PATH ",12,0,0,0);
    raw(__NR_write,2,(long)original,length(original,PATH_LIMIT),0,0,0);
    raw(__NR_write,2,(long)" -> ",4,0,0,0);
    raw(__NR_write,2,(long)mapped,length(mapped,PATH_LIMIT),0,0,0);
    raw(__NR_write,2,(long)"\n",1,0,0,0);
#endif
    long a[6];for(unsigned i=0;i<6;i++)a[i]=r[i];a[argument]=(long)mapped;
    if(nr==__NR_readlinkat) {
        if(!r[3] || r[3]>INT32_MAX) {r[0]=(uint64_t)-EINVAL;return;}
        char value[PATH_LIMIT];status=raw(nr,a[0],a[1],(long)value,sizeof(value),0,0);
        if(status<0) {r[0]=status;return;}
        const char* visible=value;size_t size=(size_t)status;
        if(size>=root_size && equal(value,pocket_loader_config.root,root_size) &&
           (size==root_size || value[root_size]=='/')) {
            visible=value+root_size;size-=root_size;
            if(!size) {visible="/";size=1;}
        }
        if(size>r[3])size=r[3];
        r[0]=copy_to_user(r[2],visible,size);return;
    }
    if(nr==__NR_execve || nr==__NR_execveat) {
        if(nr==__NR_execveat && a[4]) {r[0]=(uint64_t)-EOPNOTSUPP;return;}
        status=prepared_exec(nr==__NR_execve?AT_FDCWD:(int)a[0],mapped);
        if(status<0) {r[0]=status;return;}
        // Exec preserves signal masks. Restore the pre-trap mask before losing
        // this signal frame, so SIGSYS remains usable in the next interpreter.
        status=raw(__NR_rt_sigprocmask,SIG_SETMASK,(long)&context->uc_sigmask,0,8,0,0);
        if(status<0) {r[0]=status;return;}
    }
    r[0]=raw(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
}
static void install_handler(void) {
    struct KernelAction {void (*handler)(int,siginfo_t*,void*);unsigned long flags;void (*restorer)(void);unsigned long mask;};
    struct KernelAction action={trapped,SA_SIGINFO|0x04000000UL,pocket_restore_rt,0};
    long result=raw(__NR_rt_sigaction,SIGSYS,(long)&action,0,8,0,0);
    if(result<0)fail("sigaction",result);
}
static void install_filter(void) {
    struct sock_filter code[80];unsigned used=0;
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
    JUMP(BPF_JMP|BPF_JEQ|BPF_K,__NR_getpid,0,4);
    STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args));
    JUMP(BPF_JMP|BPF_JEQ|BPF_K,PROBE_INPUT,0,1);
    STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP|FILTER_MARKER);
    STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW);
    const unsigned numbers[]={__NR_openat,__NR_newfstatat,__NR_faccessat,__NR_readlinkat,
        __NR_execve,__NR_execveat,__NR_rt_sigaction};
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
static uintptr_t load_linker(size_t page,uintptr_t* bias) {
    long fd=raw(__NR_openat,AT_FDCWD,(long)pocket_loader_config.linker,O_RDONLY|O_CLOEXEC,0,0,0);
    if(fd<0)fail("open linker",fd);
    Elf64_Ehdr e;Elf64_Phdr p[MAX_PHDR];
    if(raw(__NR_pread64,fd,(long)&e,sizeof(e),0,0,0)!=sizeof(e) || !elf_header(&e) || e.e_type!=ET_DYN)
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
    long reserved=raw(__NR_mmap,0,high-low,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(reserved<0)fail("reserve linker",reserved);
    *bias=(uintptr_t)reserved-low;
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
    uintptr_t argc=*stack,at=argc+2;
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
    install_handler();
    if(pocket_filter_probe(PROBE_INPUT)!=PROBE_OUTPUT)install_filter();
    if(pocket_filter_probe(PROBE_INPUT)!=PROBE_OUTPUT)fail("filter probe",-ENOSYS);
    uintptr_t bias,entry=load_linker(page,&bias);base->a_un.a_val=bias;
    return entry;
}
