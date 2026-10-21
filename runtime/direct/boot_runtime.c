// SPDX-License-Identifier: GPL-3.0-only
// Development loader for the exact pinned guest libc. Never patches host code.
#define _GNU_SOURCE
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include "guest_paths.h"
#include <android/dlext.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <grp.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/xattr.h>
#include <sys/inotify.h>
#include <unistd.h>

#define DECLARE(name) extern __typeof__(name) __wrap_##name
DECLARE(openat); DECLARE(fstatat); DECLARE(readlinkat); DECLARE(mkdirat);
DECLARE(unlinkat); DECLARE(renameat); DECLARE(linkat); DECLARE(fchownat);
DECLARE(symlinkat); DECLARE(utimensat); DECLARE(chdir); DECLARE(fchdir);
DECLARE(bind); DECLARE(connect); DECLARE(getsockname); DECLARE(getpeername);
DECLARE(faccessat); DECLARE(fchmodat); DECLARE(getcwd); DECLARE(execve);
DECLARE(getxattr); DECLARE(lgetxattr); DECLARE(setxattr); DECLARE(lsetxattr);
DECLARE(listxattr); DECLARE(llistxattr); DECLARE(removexattr); DECLARE(lremovexattr);
DECLARE(inotify_add_watch);

static uid_t ruid, euid, suid;
static gid_t rgid, egid, sgid;
static int root_capable;
static char runtime_console[PATH_MAX], runtime_parent[32];
extern int __wrap_mknodat(int, const char*, mode_t, dev_t);
// Explicit dynamic-call entry points for guest tools such as Magisk/BusyBox.
// These create real app-owned FIFOs, never substitute kernel privileges.
int mknodat(int dir, const char* path, mode_t mode, dev_t device) {
    return __wrap_mknodat(dir, path, mode, device);
}
int mknod(const char* path, mode_t mode, dev_t device) {
    return __wrap_mknodat(AT_FDCWD, path, mode, device);
}
int mkfifoat(int dir, const char* path, mode_t mode) {
    return __wrap_mknodat(dir, path, (mode & 07777) | S_IFIFO, 0);
}
int mkfifo(const char* path, mode_t mode) {
    return mkfifoat(AT_FDCWD, path, mode);
}

// Guest device events belong to a private channel, never the host's kernel
// uevent netlink socket. An explicit relay may inject records in the future.
static pthread_mutex_t event_mutex=PTHREAD_MUTEX_INITIALIZER;
static int event_pair[2]={-1,-1};
static pid_t event_pid;
int uevent_init(void) {
    pthread_mutex_lock(&event_mutex);
    if(event_pid!=getpid()) {
        if(event_pair[0]>=0)close(event_pair[0]);
        if(event_pair[1]>=0)close(event_pair[1]);
        event_pair[0]=event_pair[1]=-1;event_pid=getpid();
    }
    int ok=event_pair[0]>=0 || socketpair(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0,event_pair)==0;
    pthread_mutex_unlock(&event_mutex);return ok;
}
int uevent_get_fd(void) {return uevent_init()?event_pair[0]:-1;}
int uevent_next_event(char* buffer,int capacity) {
    if(!buffer || capacity<2) {errno=EINVAL;return -1;}
    int fd=uevent_get_fd();if(fd<0)return -1;
    ssize_t count;
    do {count=recv(fd,buffer,capacity-1,0);}while(count<0 && errno==EINTR);
    if(count>=0)buffer[count]=0;
    return count;
}
int pocket_uevent_inject(const char* buffer,int length) {
    if(!buffer || length<1 || length>4096 || buffer[length-1]) {errno=EINVAL;return -1;}
    if(!uevent_init())return -1;
    return send(event_pair[1],buffer,length,MSG_NOSIGNAL);
}

// Guest timers may expire while this app runs; waking a suspended host belongs
// to a future host AlarmManager bridge. Never request CAP_WAKE_ALARM or change
// the host wall clock/timezone on behalf of a guest.
static int boot_timerfd_create(clockid_t clock, int flags) {
    if (clock==CLOCK_REALTIME_ALARM) clock=CLOCK_REALTIME;
    if (clock==CLOCK_BOOTTIME_ALARM) clock=CLOCK_BOOTTIME;
    return syscall(SYS_timerfd_create,clock,flags);
}
static int boot_settimeofday(const struct timeval* time,const struct timezone* zone) {
    (void)time;(void)zone;errno=EPERM;return -1;
}

typedef struct { uint32_t magic; uid_t uid; gid_t gid; } Owner;
static int owner_path(const struct stat* st,char* path) {
    int n=snprintf(path,PATH_MAX,"%s/.pocket-owner/%llx-%llx",pocket_guest_root(),
                   (unsigned long long)st->st_dev,(unsigned long long)st->st_ino);
    if (n>=PATH_MAX) { errno=ENAMETOOLONG;return -1; } return 0;
}
static void owner_read(struct stat* st) {
    char path[PATH_MAX];Owner owner={0};
    st->st_uid=0;st->st_gid=0;
    if (owner_path(st,path)) return;
    int saved=errno,fd=syscall(SYS_openat,AT_FDCWD,path,O_RDONLY|O_CLOEXEC,0);
    if (fd>=0) {
        if (read(fd,&owner,sizeof(owner))==sizeof(owner) && owner.magic==0x504f434b) {
            st->st_uid=owner.uid;st->st_gid=owner.gid;
        }
        close(fd);
    }
    errno=saved;
}
static int owner_write(const struct stat* st,uid_t uid,gid_t gid) {
    char path[PATH_MAX],directory[PATH_MAX],temporary[PATH_MAX];
    if (owner_path(st,path)) return -1;
    snprintf(directory,sizeof(directory),"%s/.pocket-owner",pocket_guest_root());
    if (syscall(SYS_mkdirat,AT_FDCWD,directory,0700) && errno!=EEXIST) return -1;
    if (snprintf(temporary,sizeof(temporary),"%s.%d.%ld.tmp",path,getpid(),syscall(SYS_gettid)) >=(int)sizeof(temporary)) { errno=ENAMETOOLONG;return -1; }
    int fd=syscall(SYS_openat,AT_FDCWD,temporary,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);
    if (fd<0) return -1;
    Owner owner={0x504f434b,uid,gid};
    int rc=write(fd,&owner,sizeof(owner))==sizeof(owner)?0:-1,e=errno;
    close(fd);errno=e;
    if (!rc) rc=syscall(SYS_renameat,AT_FDCWD,temporary,AT_FDCWD,path);
    e=errno;syscall(SYS_unlinkat,AT_FDCWD,temporary,0);errno=e;return rc;
}
int pocket_boot_created_at(int dir,const char* path) {
    struct stat st;
    if (syscall(SYS_newfstatat,dir,path,&st,AT_SYMLINK_NOFOLLOW)) return -1;
    return owner_write(&st,euid,egid);
}

int pocket_boot_passthrough(const char* path) {
    if (!strncmp(path, "/proc", 5) && (!path[5] || path[5] == '/')) return 1;
    if (!strncmp(path, "/sys", 4) && (!path[4] || path[4] == '/')) return 1;
    // PTYs are allocated by the host kernel with the ordinary host app UID.
    // Access remains subject to its SELinux domain and kernel permissions.
    if (!strncmp(path, "/dev/pts", 8) && (!path[8] || path[8] == '/')) return 1;
    const char* nodes[] = {"/dev/null", "/dev/zero", "/dev/random", "/dev/urandom", "/dev/ptmx", "/dev/tty", "/dev/binder", "/dev/binderfs/binder", "/dev/ashmem", NULL};
    for (int n = 0; nodes[n]; ++n) if (!strcmp(path, nodes[n])) return 1;
    return 0;
}
static void identity_environment(void) {
    char identity[64]; snprintf(identity, sizeof(identity), "%u:%u", euid, egid);
    if (setenv("POCKET_DIRECT_ID", identity, 1)) _exit(126);
}
static uid_t boot_getuid(void) { return ruid; }
static uid_t boot_geteuid(void) { return euid; }
static gid_t boot_getgid(void) { return rgid; }
static gid_t boot_getegid(void) { return egid; }
static int boot_getresuid(uid_t* r, uid_t* e, uid_t* s) { *r=ruid; *e=euid; *s=suid; return 0; }
static int boot_getresgid(gid_t* r, gid_t* e, gid_t* s) { *r=rgid; *e=egid; *s=sgid; return 0; }
static int boot_setresuid(uid_t r, uid_t e, uid_t s) {
    uid_t values[] = {r,e,s};
    for (int n=0;n<3;++n) if (!root_capable && values[n]!=(uid_t)-1 && values[n]!=ruid && values[n]!=euid && values[n]!=suid) { errno=EPERM; return -1; }
    if (r!=(uid_t)-1) ruid=r; if (e!=(uid_t)-1) euid=e; if (s!=(uid_t)-1) suid=s;
    root_capable=euid==0;
    identity_environment(); return 0;
}
static int boot_setresgid(gid_t r, gid_t e, gid_t s) {
    gid_t values[] = {r,e,s};
    for (int n=0;n<3;++n) if (!root_capable && values[n]!=(gid_t)-1 && values[n]!=rgid && values[n]!=egid && values[n]!=sgid) { errno=EPERM; return -1; }
    if (r!=(gid_t)-1) rgid=r; if (e!=(gid_t)-1) egid=e; if (s!=(gid_t)-1) sgid=s;
    identity_environment(); return 0;
}
static int boot_setuid(uid_t id) { return root_capable ? boot_setresuid(id,id,id) : boot_setresuid(-1,id,-1); }
static int boot_setgid(gid_t id) { return root_capable ? boot_setresgid(id,id,id) : boot_setresgid(-1,id,-1); }
static int boot_setreuid(uid_t r, uid_t e) { return boot_setresuid(r,e,e==(uid_t)-1?(uid_t)-1:e); }
static int boot_setregid(gid_t r, gid_t e) { return boot_setresgid(r,e,e==(gid_t)-1?(gid_t)-1:e); }
static gid_t guest_groups[64];
static size_t guest_group_count;
int pocket_boot_exec_value(const char* key,char* value,size_t capacity) {
    int count;
    if(!strcmp(key,"POCKET_DIRECT_ID")) count=snprintf(value,capacity,"%u:%u",euid,egid);
    else if(!strcmp(key,"POCKET_DIRECT_GROUPS")) {
        size_t used=0;
        if(!capacity) {errno=ENAMETOOLONG;return -1;}
        value[0]=0;
        for(size_t i=0;i<guest_group_count;++i) {
            count=snprintf(value+used,capacity-used,"%s%u",i?",":"",guest_groups[i]);
            if(count<0 || (size_t)count>=capacity-used) {errno=ENAMETOOLONG;return -1;}
            used+=count;
        }
        return 1;
    } else {
        const char* saved=!strcmp(key,"POCKET_DIRECT_CONSOLE")?runtime_console:
            !strcmp(key,"POCKET_PARENT_PID")?runtime_parent:NULL;
        if(!saved || !*saved)return 0;
        count=snprintf(value,capacity,"%s",saved);
    }
    if(count<0 || (size_t)count>=capacity) {errno=ENAMETOOLONG;return -1;}
    return 1;
}
static int boot_setgroups(size_t n, const gid_t* groups) {
    if (!root_capable) { errno=EPERM; return -1; }
    if (n>64) { errno=EINVAL; return -1; }
    if (n && !groups) { errno=EFAULT; return -1; }
    if (n) memcpy(guest_groups,groups,n*sizeof(gid_t));
    guest_group_count=n;
    char text[64*12+1], *cursor=text;
    for (size_t i=0;i<n;++i) cursor+=sprintf(cursor,"%s%u",i?",":"",groups[i]);
    *cursor=0;
    if (setenv("POCKET_DIRECT_GROUPS",text,1)) return -1;
    return 0;
}
static int boot_getgroups(int n, gid_t* groups) {
    if (n<0 || (n && (size_t)n<guest_group_count)) { errno=EINVAL; return -1; }
    if (n && guest_group_count) {
        if (!groups) { errno=EFAULT; return -1; }
        memcpy(groups,guest_groups,guest_group_count*sizeof(gid_t));
    }
    return (int)guest_group_count;
}
static int boot_getcwd(char* path, size_t size) { return __wrap_getcwd(path,size) ? (int)strlen(path)+1 : -1; }
static int private_fd(int fd) {
    char proc[64], path[PATH_MAX]; snprintf(proc,sizeof(proc),"/proc/self/fd/%d",fd);
    ssize_t n=syscall(SYS_readlinkat,AT_FDCWD,proc,path,sizeof(path)-1);
    if (n<0) return 0; path[n]=0;
    const char* root=pocket_guest_root(); size_t prefix=strlen(root);
    return !strncmp(path,root,prefix) && (!path[prefix] || path[prefix]=='/');
}
static int boot_fstat(int fd, struct stat* st) {
    int rc=syscall(SYS_fstat,fd,st);
    if (!rc && private_fd(fd)) owner_read(st);
    return rc;
}
static int boot_fstatat(int dir,const char* path,struct stat* st,int flags) {
    int rc=__wrap_fstatat(dir,path,st,flags);
    if (!rc && !pocket_boot_passthrough(path)) owner_read(st);
    return rc;
}
static int may_change_owner(const struct stat* st,uid_t uid,gid_t gid) {
    if(root_capable) return 1;
    if(st->st_uid!=euid || (uid!=(uid_t)-1 && uid!=st->st_uid)) return 0;
    if(gid==(gid_t)-1 || gid==st->st_gid || gid==egid) return 1;
    for(size_t n=0;n<guest_group_count;++n) if(guest_groups[n]==gid) return 1;
    return 0;
}
static int boot_chown(int dir,const char* path,uid_t uid,gid_t gid,int flags) {
    if (flags & ~AT_SYMLINK_NOFOLLOW || pocket_boot_passthrough(path)) { errno=ENOTSUP;return -1; }
    struct stat st;if (__wrap_fstatat(dir,path,&st,flags)) return -1;
    owner_read(&st);
    if (!may_change_owner(&st,uid,gid)) { errno=EPERM;return -1; }
    return owner_write(&st,uid==(uid_t)-1?st.st_uid:uid,gid==(gid_t)-1?st.st_gid:gid);
}
static int boot_fchown(int fd,uid_t uid,gid_t gid) {
    if (!private_fd(fd)) return syscall(SYS_fchown,fd,uid,gid);
    int flags=fcntl(fd,F_GETFL);
    if (flags<0) return -1;
    if (flags&O_PATH) { errno=EBADF;return -1; }
    struct stat st;if (syscall(SYS_fstat,fd,&st)) return -1;
    owner_read(&st);
    if (!may_change_owner(&st,uid,gid)) { errno=EPERM;return -1; }
    return owner_write(&st,uid==(uid_t)-1?st.st_uid:uid,gid==(gid_t)-1?st.st_gid:gid);
}
static int boot_chmod(int dir,const char* path,mode_t mode,int flags) {
    if (!flags) return __wrap_fchmodat(dir,path,mode,0);
    if (flags!=AT_SYMLINK_NOFOLLOW) { errno=ENOTSUP;return -1; }
    int fd=__wrap_openat(dir,path,O_PATH|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return -1;
    struct stat st;int rc=syscall(SYS_fstat,fd,&st);
    if (!rc && S_ISLNK(st.st_mode)) { errno=ENOTSUP;rc=-1; }
    if (!rc) {
        char proc[64];snprintf(proc,sizeof(proc),"/proc/self/fd/%d",fd);
        rc=syscall(SYS_fchmodat,AT_FDCWD,proc,mode);
    }
    int e=errno;close(fd);errno=e;return rc;
}
static int boot_statfs(const char* path,struct statfs* st) {
    int fd=__wrap_openat(AT_FDCWD,path,O_PATH|O_CLOEXEC);
    if (fd<0) return -1; int rc=syscall(SYS_fstatfs,fd,st),e=errno; close(fd);errno=e;return rc;
}
// The non-boot passthrough branches of guest_paths.c need the original symbols.
char* __real_realpath(const char* p,char* out) { char*(*fn)(const char*,char*)=dlsym(RTLD_NEXT,"realpath"); return fn(p,out); }
int __real_faccessat(int fd,const char* p,int mode,int flags) { (void)flags; return syscall(SYS_faccessat,fd,p,mode); }
int __real_fchmodat(int fd,const char* p,mode_t mode,int flags) { (void)flags; return syscall(SYS_fchmodat,fd,p,mode); }

void* dlopen(const char* file,int flags) {
    void*(*real)(const char*,int)=dlsym(RTLD_NEXT,"dlopen");
    char mapped[PATH_MAX];
    if (file && file[0]=='/' && !pocket_boot_passthrough(file)) {
        if (pocket_boot_map_path(file,mapped)) return NULL;
        file=mapped;
    }
    return real(file,flags);
}
void* android_dlopen_ext(const char* file,int flags,const android_dlextinfo* info) {
    void*(*real)(const char*,int,const android_dlextinfo*)=dlsym(RTLD_NEXT,"android_dlopen_ext");
    char mapped[PATH_MAX];
    if (file && file[0]=='/' && !pocket_boot_passthrough(file)) {
        if (pocket_boot_map_path(file,mapped)) return NULL;
        file=mapped;
    }
    return real(file,flags,info);
}

// Minimal crash diagnostics for the development profile. Preserve fatal signals;
// no debugger, signal recovery, or changes to host crash/security policy.
static char* crash_hex(char* out,uintptr_t value) {
    static const char digits[]="0123456789abcdef";
    for (int i=15;i>=0;--i) *out++=digits[(value>>(i*4))&15];
    return out;
}
static void crash_trace(int number,siginfo_t* info,void* context) {
    ucontext_t* uc=context;
    char line[256],*p=line;
    const char prefix[]="DIRECT_CRASH signal=0x";memcpy(p,prefix,sizeof(prefix)-1);p+=sizeof(prefix)-1;
    p=crash_hex(p,number);memcpy(p," pc=0x",6);p+=6;
#if defined(__aarch64__)
    p=crash_hex(p,uc->uc_mcontext.pc);
    memcpy(p," lr=0x",6);p+=6;p=crash_hex(p,uc->uc_mcontext.regs[30]);
#elif defined(__x86_64__)
    p=crash_hex(p,uc->uc_mcontext.gregs[REG_RIP]);
    memcpy(p," sp=0x",6);p+=6;p=crash_hex(p,uc->uc_mcontext.gregs[REG_RSP]);
#else
#error "crash_trace: unsupported architecture"
#endif
    memcpy(p," address=0x",11);p+=11;p=crash_hex(p,(uintptr_t)info->si_addr);*p++='\n';
    syscall(SYS_write,STDERR_FILENO,line,p-line);
    int fd=syscall(SYS_openat,AT_FDCWD,"/proc/self/maps",O_RDONLY|O_CLOEXEC,0);
    if (fd>=0) {
        char buffer[4096];long n;size_t total=0;
        while (total<65536 && (n=syscall(SYS_read,fd,buffer,sizeof(buffer)))>0) {
            syscall(SYS_write,STDERR_FILENO,buffer,n);total+=n;
        }
        syscall(SYS_close,fd);
    }
    syscall(SYS_tgkill,getpid(),syscall(SYS_gettid),number);
}
typedef struct { const char* name; uintptr_t offset; unsigned char expected[16]; void* target; } Hook;
#include "boot_hooks.h"

static long current_tracer(void) {
    char buffer[2048];int fd=syscall(SYS_openat,AT_FDCWD,"/proc/self/status",O_RDONLY|O_CLOEXEC,0);
    if (fd<0) return -1;
    long size=syscall(SYS_read,fd,buffer,sizeof(buffer)-1);syscall(SYS_close,fd);
    if (size<0) return -1;buffer[size]=0;
    char* field=strstr(buffer,"TracerPid:");return field?strtol(field+10,NULL,10):-1;
}
__attribute__((constructor)) static void boot_initialize(void) {
    const char* root=pocket_guest_root();
    if (!root) _exit(125);
    const char* console=getenv("POCKET_DIRECT_CONSOLE");
    if(console && strlen(console)<sizeof(runtime_console) && !strncmp(console,root,strlen(root)) && console[strlen(root)]=='/')
        strcpy(runtime_console,console);
    const char* parent=getenv("POCKET_PARENT_PID");
    if(parent && strlen(parent)<sizeof(runtime_parent))strcpy(runtime_parent,parent);
    struct sigaction crash={0};crash.sa_sigaction=crash_trace;crash.sa_flags=SA_SIGINFO|SA_RESETHAND;
    sigemptyset(&crash.sa_mask);sigaction(SIGSEGV,&crash,NULL);sigaction(SIGABRT,&crash,NULL);
    const char* identity=getenv("POCKET_DIRECT_ID");
    unsigned uid=0,gid=0;
    if (identity && sscanf(identity,"%u:%u",&uid,&gid)!=2) _exit(125);
    ruid=euid=suid=uid; rgid=egid=sgid=gid; root_capable=uid==0;
    const char* inherited_groups=getenv("POCKET_DIRECT_GROUPS");
    if (inherited_groups && *inherited_groups) {
        char* copy=strdup(inherited_groups), *save=NULL;
        if (!copy) _exit(125);
        for (char* part=strtok_r(copy,",",&save);part;part=strtok_r(NULL,",",&save)) {
            char* end=NULL; errno=0; unsigned long group=strtoul(part,&end,10);
            if (errno || !*part || *end || group>UINT32_MAX || guest_group_count==64) _exit(125);
            guest_groups[guest_group_count++]=(gid_t)group;
        }
        free(copy);
    }
    Dl_info image;
    if (!dladdr((void*)syscall,&image) || strncmp(image.dli_fname,root,strlen(root)) ||
        strcmp(image.dli_fname+strlen(root),"/system/lib64/libc.so")) _exit(125);
    unsigned char* base=image.dli_fbase;
    long page=sysconf(_SC_PAGESIZE);
    for (size_t n=0;n<sizeof(hooks)/sizeof(hooks[0]);++n) {
        Hook* h=&hooks[n]; unsigned char* address=base+h->offset;
        if (memcmp(address,h->expected,16)) { fprintf(stderr,"BOOT_HOOK_MISMATCH %s\n",h->name); _exit(125); }
        uintptr_t begin=(uintptr_t)address & ~((uintptr_t)page-1);
        size_t size=((uintptr_t)address+16-begin+page-1)&~((uintptr_t)page-1);
        if (mprotect((void*)begin,size,PROT_READ|PROT_WRITE|PROT_EXEC)) { perror("BOOT_HOOK_MPROTECT");_exit(125); }
#if defined(__aarch64__)
        uint32_t jump[2]={0x58000050,0xd61f0200}; // ldr x16, +8; br x16
        memcpy(address,jump,8);memcpy(address+8,&h->target,8);
#elif defined(__x86_64__)
        // RIP-relative indirect jump preserves every argument register,
        // including AL for variadic calls. The target follows the instruction.
        const unsigned char jump[6]={0xff,0x25,0,0,0,0};
        memcpy(address,jump,sizeof(jump));
        memcpy(address+sizeof(jump),&h->target,sizeof(h->target));
#else
#error "boot hooks: unsupported architecture"
#endif
        __builtin___clear_cache((char*)address,(char*)address+16);
        if (mprotect((void*)begin,size,PROT_READ|PROT_EXEC)) _exit(125);
    }
    // Fresh private property state in this process's pinned guest libc. Startup
    // may have mapped host properties before our constructor; those read-only
    // bootstrap mappings are left mapped, never written. No guest threads exist.
    memset(base+PROPERTY_STATE_OFFSET,0,PROPERTY_STATE_SIZE);
    extern int __system_properties_init(void);
    int result=__system_properties_init();
    // App commands may capture stderr as data (for example `id -u 2>&1`).
    // Keep runtime diagnostics in the session log instead of their pipes.
    int saved=errno,diagnostics=STDERR_FILENO;
    if(console) {
        size_t prefix=strlen(root);
        diagnostics=(!strncmp(console,root,prefix) && console[prefix]=='/') ?
            syscall(SYS_openat,AT_FDCWD,console,O_WRONLY|O_APPEND|O_CLOEXEC,0) : -1;
    }
    if(diagnostics>=0) {
        dprintf(diagnostics,"DIRECT_BOOT_READY hooks=%zu kernel_uid=%ld guest_uid=%u properties_init=%d tracer_pid=%ld\n",
                sizeof(hooks)/sizeof(hooks[0]),syscall(SYS_getuid),euid,result,current_tracer());
        if(console)close(diagnostics);
    }
    errno=saved;
}
