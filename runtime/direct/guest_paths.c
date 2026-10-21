// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include "guest_paths.h"
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/xattr.h>
#include <sys/inotify.h>
#include <unistd.h>

#if defined(SYS_newfstatat)
#define POCKET_FSTATAT SYS_newfstatat
#else
#define POCKET_FSTATAT SYS_fstatat64
#endif

static pthread_once_t once = PTHREAD_ONCE_INIT;
static char root[PATH_MAX];
static int init_error;
static bool enabled;
#ifdef POCKET_DIRECT_BOOT
static char data_alias[NAME_MAX+1];
static bool module_view;
static bool module_view_ready(void) {
    if (!module_view && root[0]) {
        char marker[PATH_MAX]; struct stat st;
        int n=snprintf(marker,sizeof(marker),"%s/.pocket-module-view/ready",root);
        if (n>0 && n<(int)sizeof(marker) && !syscall(POCKET_FSTATAT,AT_FDCWD,marker,&st,AT_SYMLINK_NOFOLLOW))
            module_view=S_ISREG(st.st_mode);
    }
    return module_view;
}
static void visible_name(const char* path,char* output) {
    if (!strncmp(path,"/.pocket-module-view/",21)) path+=20;
    size_t length=strlen(data_alias);
    if(length && path[0]=='/' && !strncmp(path+1,data_alias,length) && (!path[length+1] || path[length+1]=='/')) {
        strcpy(output,"/data");strcat(output,path+length+1);
    } else strcpy(output,path);
}
#endif

static int raw_open(int dir, const char* path, int flags, mode_t mode) {
    return (int)syscall(SYS_openat, dir, path, flags, mode);
}
static int raw_stat(int dir, const char* path, struct stat* st, int flags) {
    return (int)syscall(POCKET_FSTATAT, dir, path, st, flags);
}
static int fd_path(int fd, char* path) {
    char proc[64];
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    ssize_t n = syscall(SYS_readlinkat, AT_FDCWD, proc, path, PATH_MAX - 1);
    if (n < 0) return -1;
    if (n == PATH_MAX - 1) { errno = ENAMETOOLONG; return -1; }
    path[n] = 0; return 0;
}
static void initialize(void) {
    const char* value = getenv("POCKET_DIRECT_ROOT");
    if (!value) return; // Same controlled binary can run under PRoot unchanged.
    enabled = true;
    if (!*value || value[0] != '/') { init_error = EINVAL; return; }
    int root_fd = raw_open(AT_FDCWD, value, O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
    if (root_fd < 0) { init_error = errno; return; }
    // Do not call wrapped realpath during pthread_once initialization.
    if (fd_path(root_fd, root) || !strcmp(root, "/")) {
        init_error = errno ? errno : EINVAL;
    }
#ifdef POCKET_DIRECT_BOOT
    if(!init_error) {
        char target[NAME_MAX+1];struct stat st;
        ssize_t n=syscall(SYS_readlinkat,root_fd,"data",target,sizeof(target)-1);
        if(n>0) {
            target[n]=0;
            if((!strcmp(target,"system-data") || !strcmp(target,"zygote-data")) &&
               raw_stat(root_fd,target,&st,AT_SYMLINK_NOFOLLOW)==0 && S_ISDIR(st.st_mode)) strcpy(data_alias,target);
        }
    }
#endif
    close(root_fd);
}
int pocket_guest_init(void) {
    pthread_once(&once, initialize);
    if (init_error) { errno = init_error; return -1; }
    return 0;
}
const char* pocket_guest_root(void) {
    return pocket_guest_init() || !enabled ? NULL : root;
}

// Translate kernel cwd/dirfd identity back to a guest path. Foreign directory
// descriptors fail explicitly instead of accidentally resolving host paths.
static int directory_name(int dir, char* guest) {
    char actual[PATH_MAX];
    if (dir == AT_FDCWD) {
        if (syscall(SYS_getcwd, actual, sizeof(actual)) < 0) return -1;
    } else {
        struct stat st;
        if (syscall(SYS_fstat, dir, &st)) return -1;
        if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
        if (fd_path(dir, actual)) return -1;
    }
    size_t n = strlen(root);
    if (strncmp(actual, root, n) || (actual[n] && actual[n] != '/')) {
#ifdef POCKET_DIRECT_BOOT
        if (pocket_boot_passthrough(actual)) { strcpy(guest, actual); return 0; }
#endif
        errno = EXDEV; return -1;
    }
#ifdef POCKET_DIRECT_BOOT
    // dirfd-relative operations must retain the physical view identity.
    // Converting it to /system here would redirect view cleanup to the base
    // image before publication. Only getcwd/realpath presentation hides it.
    if (!strncmp(actual+n,"/.pocket-module-view/",21)) strcpy(guest,actual+n);
    else visible_name(actual[n]?actual+n:"/",guest);
#else
    strcpy(guest, actual[n] ? actual + n : "/");
#endif
    return 0;
}

typedef struct { int dir; char name[NAME_MAX + 1]; bool slash; } Path;

// Walk through real directory FDs, interpreting absolute symlinks relative to
// the guest root. Preserve symlink/.. ordering, clamp .. at root, and cap link
// traversal. This is compatibility logic, not a security boundary against code
// issuing raw syscalls, changing this process's memory, or racing renames.
static int resolve(int dir, const char* path, bool follow, Path* result) {
    if (pocket_guest_init()) return -1;
    if (!path) { errno = EFAULT; return -1; }
    size_t length = strlen(path);
    if (!length) { errno = ENOENT; return -1; }
    if (length >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    result->slash = path[length - 1] == '/';
    if (!enabled) {
        // Passthrough is handled in wrappers; never truncate a long leaf here.
        errno = ENOTSUP; return -1;
    }
#ifdef POCKET_DIRECT_BOOT
    // The kernel and loader expose real paths to our own mapped files.
    size_t prefix = strlen(root);
    if (!strncmp(path, root, prefix) && (path[prefix] == '/' || !path[prefix]))
        path = path[prefix] ? path + prefix : "/";
#endif
    char todo[PATH_MAX], base[PATH_MAX];
    if (*path == '/') strcpy(todo, path);
    else {
        if (directory_name(dir, base)) return -1;
        if (snprintf(todo, sizeof(todo), "%s/%s", base, path) >= (int)sizeof(todo)) {
            errno = ENAMETOOLONG; return -1;
        }
    }
    int stack[PATH_MAX / 2], depth = 0, links = 0;
#ifdef POCKET_DIRECT_BOOT
    bool host_tree = pocket_boot_passthrough(todo);
    stack[0] = raw_open(AT_FDCWD, host_tree ? "/" : root, O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
#else
    stack[0] = raw_open(AT_FDCWD, root, O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
#endif
    if (stack[0] < 0) return -1;
    int saved = 0;
    for (;;) {
        char* start = todo;
        while (*start == '/') ++start;
        if (!*start) {
            strcpy(result->name, ".");
            break;
        }
        char* end = strchr(start, '/');
        size_t size = end ? (size_t)(end - start) : strlen(start);
        if (size > NAME_MAX) { saved = ENAMETOOLONG; break; }
        char part[NAME_MAX + 1];
        memcpy(part, start, size); part[size] = 0;
        memmove(todo, end ? end : start + size, strlen(end ? end : start + size) + 1);
        const char* rest = todo;
        while (*rest == '/') ++rest;
        bool last = !*rest;
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) { if (depth) close(stack[depth--]); continue; }
#ifdef POCKET_DIRECT_BOOT
        // Present the selected private data directory as a virtual mount at
        // /data, including lstat/realpath semantics. Its host-side symlink is
        // an implementation detail, not part of the guest filesystem view.
        if(!host_tree && depth==0 && data_alias[0] && !strcmp(part,"data")) {
            int next=raw_open(stack[depth],data_alias,O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0);
            if(next<0) {saved=errno;break;}
            stack[++depth]=next;continue;
        }
        // Magic Mount compatibility: a fully merged private tree supplies
        // directory enumeration as well as open/stat/exec/dlopen semantics.
        // Absolute symlinks still restart at the virtual guest root.
        if (!host_tree && depth==0 && module_view_ready() &&
            (!strcmp(part,"system") || !strcmp(part,"vendor"))) {
            char view[64]; snprintf(view,sizeof(view),".pocket-module-view/%s",part);
            int next=raw_open(stack[0],view,O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0);
            if(next>=0) {
                if(last) { close(stack[0]); stack[0]=next; strcpy(result->name,"."); break; }
                stack[++depth]=next;continue;
            }
            if(errno!=ENOENT) {saved=errno;break;}
        }
#endif
        // Read metadata without following the final link in the host namespace.
        struct stat st;
        if (raw_stat(stack[depth], part, &st, AT_SYMLINK_NOFOLLOW)) {
            if (last && errno == ENOENT) { strcpy(result->name, part); break; }
            saved = errno; break;
        }
        if (S_ISLNK(st.st_mode) && (!last || follow || result->slash)) {
            if (++links > 40) { saved = ELOOP; break; }
            char target[PATH_MAX], combined[PATH_MAX];
            ssize_t n = syscall(SYS_readlinkat, stack[depth], part, target, sizeof(target) - 1);
            if (n < 0) { saved = errno; break; }
            target[n] = 0;
            if (last && n && target[n - 1] == '/') result->slash = true;
            if (snprintf(combined, sizeof(combined), "%s%s", target, todo) >= (int)sizeof(combined)) {
                saved = ENAMETOOLONG; break;
            }
            if (target[0] == '/') while (depth) close(stack[depth--]);
            strcpy(todo, combined);
            continue;
        }
        if (last) { strcpy(result->name, part); break; }
        int next = raw_open(stack[depth], part, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
        if (next < 0) { saved = errno; break; }
        stack[++depth] = next;
    }
    result->dir = stack[depth];
    for (int n = 0; n < depth; ++n) close(stack[n]);
    if (saved) { close(result->dir); errno = saved; return -1; }
    return 0;
}
static int finish(Path* p, int value) { int e = errno; close(p->dir); errno = e; return value; }
static int active(void) { return pocket_guest_init() ? -1 : enabled; }

static int require_directory(Path* p) {
    if (!p->slash) return 0;
    struct stat st;
    if (raw_stat(p->dir, p->name, &st, 0)) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    return 0;
}
static int host_path(Path* p, char* path) {
    char parent[PATH_MAX];
    if (fd_path(p->dir, parent)) return -1;
    if (snprintf(path, PATH_MAX, "%s/%s", parent, p->name) >= PATH_MAX) {
        errno = ENAMETOOLONG; return -1;
    }
    return 0;
}
#ifdef POCKET_DIRECT_BOOT
int pocket_boot_map_path(const char* path, char* output) {
    Path p; if (resolve(AT_FDCWD, path, true, &p)) return -1;
    return finish(&p, host_path(&p, output));
}
#endif

#ifdef POCKET_DIRECT_BOOT
// Keep the resolved parent FD alive for path-based xattrs. Kernel attributes
// are stored on the actual private inode; no host SELinux labels are rewritten.
int __wrap_inotify_add_watch(int fd,const char* path,uint32_t mask) {
    int state=active();if(state<0) return -1;
    if(!state) return syscall(SYS_inotify_add_watch,fd,path,mask);
    Path p;
    if(resolve(AT_FDCWD,path,!(mask&IN_DONT_FOLLOW),&p)) return -1;
    if(require_directory(&p)) return finish(&p,-1);
    char actual[PATH_MAX+64];
    int length=snprintf(actual,sizeof(actual),"/proc/self/fd/%d/%s",p.dir,p.name);
    if(length<0 || (size_t)length>=sizeof(actual)) {errno=ENAMETOOLONG;return finish(&p,-1);}
    return finish(&p,syscall(SYS_inotify_add_watch,fd,actual,mask));
}
static long path_xattr(long number,const char* path,bool follow,
                       uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d) {
    int state=active();if (state<0) return -1;
    if (!state) return syscall(number,path,a,b,c,d);
    Path p;if (resolve(AT_FDCWD,path,follow,&p)) return -1;
    if (require_directory(&p)) return finish(&p,-1);
    char resolved[PATH_MAX];
    if (snprintf(resolved,sizeof(resolved),"/proc/self/fd/%d/%s",p.dir,p.name)>=(int)sizeof(resolved)) {
        errno=ENAMETOOLONG;return finish(&p,-1);
    }
    return finish(&p,syscall(number,resolved,a,b,c,d));
}
ssize_t __wrap_getxattr(const char* p,const char* name,void* value,size_t size) {
    return path_xattr(SYS_getxattr,p,true,(uintptr_t)name,(uintptr_t)value,size,0);
}
ssize_t __wrap_lgetxattr(const char* p,const char* name,void* value,size_t size) {
    return path_xattr(SYS_lgetxattr,p,false,(uintptr_t)name,(uintptr_t)value,size,0);
}
int __wrap_setxattr(const char* p,const char* name,const void* value,size_t size,int flags) {
    return path_xattr(SYS_setxattr,p,true,(uintptr_t)name,(uintptr_t)value,size,flags);
}
int __wrap_lsetxattr(const char* p,const char* name,const void* value,size_t size,int flags) {
    return path_xattr(SYS_lsetxattr,p,false,(uintptr_t)name,(uintptr_t)value,size,flags);
}
ssize_t __wrap_listxattr(const char* p,char* list,size_t size) {
    return path_xattr(SYS_listxattr,p,true,(uintptr_t)list,size,0,0);
}
ssize_t __wrap_llistxattr(const char* p,char* list,size_t size) {
    return path_xattr(SYS_llistxattr,p,false,(uintptr_t)list,size,0,0);
}
int __wrap_removexattr(const char* p,const char* name) {
    return path_xattr(SYS_removexattr,p,true,(uintptr_t)name,0,0,0);
}
int __wrap_lremovexattr(const char* p,const char* name) {
    return path_xattr(SYS_lremovexattr,p,false,(uintptr_t)name,0,0,0);
}
#endif

int __wrap_openat(int dir, const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args, flags); mode = (mode_t)va_arg(args, int); va_end(args);
    }
    int state = active(); if (state < 0) return -1;
    if (!state) return raw_open(dir, path, flags, mode);
    if ((flags & O_TMPFILE) == O_TMPFILE) { errno = ENOTSUP; return -1; }
#ifdef POCKET_DIRECT_BOOT
    // Separate hwbinder userspace endpoint; the app is allowed to use its normal
    // Binder driver. Never open the host hardware-service context or register it.
    if (!strcmp(path,"/dev/hwbinder")) return raw_open(AT_FDCWD,"/dev/binder",flags,mode);
#endif
    Path p;
    if (resolve(dir, path, !(flags & O_NOFOLLOW) && !((flags & O_CREAT) && (flags & O_EXCL)), &p)) return -1;
#ifdef POCKET_DIRECT_BOOT
    struct stat before;
    bool created = (flags & O_CREAT) && raw_stat(p.dir,p.name,&before,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;
#endif
    int fd=raw_open(p.dir,p.name,flags | (p.slash ? O_DIRECTORY : 0),mode);
#ifdef POCKET_DIRECT_BOOT
    if (fd>=0 && created && pocket_boot_created_at(p.dir,p.name)) { int e=errno;close(fd);errno=e;fd=-1; }
#endif
    return finish(&p, fd);
}
int __wrap_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args, flags); mode = (mode_t)va_arg(args, int); va_end(args);
    }
    return __wrap_openat(AT_FDCWD, path, flags, mode);
}
int __wrap_fstatat(int dir, const char* path, struct stat* st, int flags) {
    int state = active(); if (state < 0) return -1;
    if (!state) return raw_stat(dir, path, st, flags);
    if (flags & ~AT_SYMLINK_NOFOLLOW) { errno = ENOTSUP; return -1; }
    Path p; if (resolve(dir, path, !(flags & AT_SYMLINK_NOFOLLOW), &p)) return -1;
    int rc = raw_stat(p.dir, p.name, st, flags);
    if (!rc && p.slash && !S_ISDIR(st->st_mode)) { errno = ENOTDIR; rc = -1; }
    return finish(&p, rc);
}
int __wrap_stat(const char* path, struct stat* st) { return __wrap_fstatat(AT_FDCWD, path, st, 0); }
int __wrap_lstat(const char* path, struct stat* st) { return __wrap_fstatat(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW); }
int __wrap_mknodat(int dir, const char* path, mode_t mode, dev_t device) {
    int state = active(); if (state < 0) return -1;
    if (!state) return syscall(SYS_mknodat, dir, path, mode, device);
    Path p; if (resolve(dir, path, false, &p)) return -1;
    // Use the real kernel operation: FIFO creation is unprivileged, while
    // character/block device creation retains the host's permission checks.
    int rc = syscall(SYS_mknodat, p.dir, p.name, mode, device);
#ifdef POCKET_DIRECT_BOOT
    if (!rc) rc = pocket_boot_created_at(p.dir, p.name);
#endif
    return finish(&p, rc);
}
ssize_t __wrap_readlinkat(int dir, const char* path, char* out, size_t size) {
    int state = active(); if (state < 0) return -1;
    if (!state) return syscall(SYS_readlinkat, dir, path, out, size);
    Path p; if (resolve(dir, path, false, &p)) return -1;
#ifdef POCKET_DIRECT_BOOT
    if (!strncmp(path, "/proc/", 6)) {
        char actual[PATH_MAX];
        ssize_t n = syscall(SYS_readlinkat, p.dir, p.name, actual, sizeof(actual) - 1);
        int e = errno; close(p.dir); errno = e;
        if (n < 0) return -1;
        actual[n] = 0;
        const char* visible = actual;
        const char* console = getenv("POCKET_DIRECT_CONSOLE");
        if (console && !strcmp(actual, console)) visible = "/pocket-console";
        else if (!strncmp(actual, root, strlen(root)) && actual[strlen(root)] == '/') visible += strlen(root);
        char translated[PATH_MAX];visible_name(visible,translated);
        n = strlen(translated); if ((size_t)n > size) n = size;
        memcpy(out, translated, n); return n;
    }
#endif
    ssize_t rc = syscall(SYS_readlinkat, p.dir, p.name, out, size);
    int e = errno; close(p.dir); errno = e; return rc;
}
ssize_t __wrap_readlink(const char* path, char* out, size_t size) { return __wrap_readlinkat(AT_FDCWD, path, out, size); }
int __wrap_mkdirat(int dir, const char* path, mode_t mode) {
    int state = active(); if (state < 0) return -1;
    if (!state) return syscall(SYS_mkdirat, dir, path, mode);
    Path p; if (resolve(dir, path, false, &p)) return -1;
    int rc=syscall(SYS_mkdirat,p.dir,p.name,mode);
#ifdef POCKET_DIRECT_BOOT
    if (!rc) rc=pocket_boot_created_at(p.dir,p.name);
#endif
    return finish(&p,rc);
}
int __wrap_mkdir(const char* path, mode_t mode) { return __wrap_mkdirat(AT_FDCWD, path, mode); }
int __wrap_unlinkat(int dir, const char* path, int flags) {
    int state = active(); if (state < 0) return -1;
    if (!state) return syscall(SYS_unlinkat, dir, path, flags);
    Path p; if (resolve(dir, path, false, &p)) return -1;
    // Avoid silently changing trailing-slash semantics for removal.
    if (p.slash) return finish(&p, (errno = ENOTSUP, -1));
    return finish(&p, syscall(SYS_unlinkat, p.dir, p.name, flags));
}
int __wrap_unlink(const char* path) { return __wrap_unlinkat(AT_FDCWD, path, 0); }
int __wrap_rmdir(const char* path) { return __wrap_unlinkat(AT_FDCWD, path, AT_REMOVEDIR); }
int __wrap_symlinkat(const char* target, int dir, const char* path) {
    int state = active(); if (state < 0) return -1;
    if (!state) return syscall(SYS_symlinkat, target, dir, path);
    Path p; if (resolve(dir, path, false, &p)) return -1;
    if (p.slash) return finish(&p, (errno = ENOTSUP, -1));
    // Keep guest link contents unchanged; resolve() interprets them later.
    int rc=syscall(SYS_symlinkat,target,p.dir,p.name);
#ifdef POCKET_DIRECT_BOOT
    if (!rc) rc=pocket_boot_created_at(p.dir,p.name);
#endif
    return finish(&p,rc);
}
int __wrap_symlink(const char* target, const char* path) { return __wrap_symlinkat(target, AT_FDCWD, path); }
int __wrap_chdir(const char* path) {
    int fd = __wrap_openat(AT_FDCWD, path, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = syscall(SYS_fchdir, fd), e = errno; close(fd); errno = e; return rc;
}
int __wrap_fchdir(int dir) {
    int state = active(); if (state < 0) return -1;
    char name[PATH_MAX]; if (state && directory_name(dir, name)) return -1;
    return syscall(SYS_fchdir, dir);
}
char* __wrap_getcwd(char* out, size_t size) {
    int state = active(); if (state < 0) return NULL;
    char name[PATH_MAX];
    if (state) { if (directory_name(AT_FDCWD, name)) return NULL; }
    else if (syscall(SYS_getcwd, name, sizeof(name)) < 0) return NULL;
#ifdef POCKET_DIRECT_BOOT
    if (state && !strncmp(name,"/.pocket-module-view/",21)) memmove(name,name+20,strlen(name+20)+1);
#endif
    size_t needed = strlen(name) + 1;
    if (!out) { size = size ? size : needed; out = malloc(size); if (!out) return NULL;
        if (size < needed) { free(out); errno = ERANGE; return NULL; }
    } else if (size < needed) { errno = ERANGE; return NULL; }
    memcpy(out, name, needed); return out;
}

// High-level libc entry points must be adapted too: the dynamic libc's hidden
// internal open calls do not go through this executable's --wrap targets.
FILE* __wrap_fopen(const char* path, const char* mode) {
    if (!mode || !*mode) { errno = EINVAL; return NULL; }
    int flags;
    switch (*mode) {
        case 'r': flags = O_RDONLY; break;
        case 'w': flags = O_WRONLY | O_CREAT | O_TRUNC; break;
        case 'a': flags = O_WRONLY | O_CREAT | O_APPEND; break;
        default: errno = EINVAL; return NULL;
    }
    for (const char* p = mode + 1; *p; ++p) {
        if (*p == '+') flags = (flags & ~O_ACCMODE) | O_RDWR;
        else if (*p == 'e') flags |= O_CLOEXEC;
        else if (*p == 'x') flags |= O_EXCL;
        else if (*p != 'b') { errno = ENOTSUP; return NULL; }
    }
    int fd = __wrap_open(path, flags, 0666);
    if (fd < 0) return NULL;
    FILE* stream = fdopen(fd, mode);
    if (!stream) { int e = errno; close(fd); errno = e; }
    return stream;
}
DIR* __wrap_opendir(const char* path) {
    int fd = __wrap_open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return NULL;
    DIR* directory = fdopendir(fd);
    if (!directory) { int e = errno; close(fd); errno = e; }
    return directory;
}
char* __real_realpath(const char*, char*);
char* __wrap_realpath(const char* path, char* out) {
    int state = active(); if (state < 0) return NULL;
    if (!state) return __real_realpath(path, out);
    Path p; if (resolve(AT_FDCWD, path, true, &p)) return NULL;
    struct stat st;
    if (raw_stat(p.dir, p.name, &st, 0) || require_directory(&p)) {
        finish(&p, -1); return NULL;
    }
    char base[PATH_MAX], canonical[PATH_MAX];
    int rc = directory_name(p.dir, base);
    if (!rc) {
        if (!strcmp(p.name, ".")) strcpy(canonical, base);
        else if (snprintf(canonical, sizeof(canonical), "%s%s%s", base,
                         strcmp(base, "/") ? "/" : "", p.name) >= (int)sizeof(canonical)) {
            errno = ENAMETOOLONG; rc = -1;
        }
    }
    finish(&p, rc);
    if (rc) return NULL;
#ifdef POCKET_DIRECT_BOOT
    if (!strncmp(canonical,"/.pocket-module-view/",21)) memmove(canonical,canonical+20,strlen(canonical+20)+1);
#endif
    if (!out) return strdup(canonical);
    strcpy(out, canonical); return out;
}
int __real_faccessat(int, const char*, int, int);
int __wrap_faccessat(int dir, const char* path, int mode, int flags) {
    int state = active(); if (state < 0) return -1;
    if (!state) return __real_faccessat(dir, path, mode, flags);
    // Do not probe faccessat2: older Android seccomp policies may trap it.
    if (flags) { errno = ENOTSUP; return -1; }
    Path p; if (resolve(dir, path, true, &p)) return -1;
    if (require_directory(&p)) return finish(&p, -1);
    return finish(&p, syscall(SYS_faccessat, p.dir, p.name, mode));
}
int __wrap_access(const char* path, int mode) { return __wrap_faccessat(AT_FDCWD, path, mode, 0); }
int __wrap_renameat(int olddir, const char* oldpath, int newdir, const char* newpath) {
    int state = active(); if (state < 0) return -1;
    if (!state) return syscall(SYS_renameat, olddir, oldpath, newdir, newpath);
    Path old, new;
    if (resolve(olddir, oldpath, false, &old)) return -1;
    if (resolve(newdir, newpath, false, &new)) return finish(&old, -1);
    int rc;
    if (old.slash || new.slash) { errno = ENOTSUP; rc = -1; }
    else rc = syscall(SYS_renameat, old.dir, old.name, new.dir, new.name);
    finish(&new, rc); return finish(&old, rc);
}
int __wrap_rename(const char* old, const char* new) { return __wrap_renameat(AT_FDCWD, old, AT_FDCWD, new); }
int __wrap_linkat(int olddir, const char* oldpath, int newdir, const char* newpath, int flags) {
    int state = active(); if (state < 0) return -1;
    if (!state) return syscall(SYS_linkat, olddir, oldpath, newdir, newpath, flags);
    if (flags & ~AT_SYMLINK_FOLLOW) { errno = ENOTSUP; return -1; }
    Path old, new;
    if (resolve(olddir, oldpath, flags & AT_SYMLINK_FOLLOW, &old)) return -1;
    if (resolve(newdir, newpath, false, &new)) return finish(&old, -1);
    int rc;
    if (old.slash || new.slash) { errno = ENOTSUP; rc = -1; }
    else rc = syscall(SYS_linkat, old.dir, old.name, new.dir, new.name, flags);
    finish(&new, rc); return finish(&old, rc);
}
int __wrap_link(const char* old, const char* new) { return __wrap_linkat(AT_FDCWD, old, AT_FDCWD, new, 0); }
int __real_fchmodat(int, const char*, mode_t, int);
int __wrap_fchmodat(int dir, const char* path, mode_t mode, int flags) {
    int state = active(); if (state < 0) return -1;
    if (!state) return __real_fchmodat(dir, path, mode, flags);
    if (flags) { errno = ENOTSUP; return -1; }
    Path p; if (resolve(dir, path, true, &p)) return -1;
    if (require_directory(&p)) return finish(&p, -1);
    return finish(&p, syscall(SYS_fchmodat, p.dir, p.name, mode));
}
int __wrap_chmod(const char* path, mode_t mode) { return __wrap_fchmodat(AT_FDCWD, path, mode, 0); }
int __wrap_fchownat(int dir, const char* path, uid_t uid, gid_t gid, int flags) {
    int state = active(); if (state < 0) return -1;
    if (!state) return syscall(SYS_fchownat, dir, path, uid, gid, flags);
    if (flags & ~AT_SYMLINK_NOFOLLOW) { errno = ENOTSUP; return -1; }
    Path p; if (resolve(dir, path, !(flags & AT_SYMLINK_NOFOLLOW), &p)) return -1;
    if (require_directory(&p)) return finish(&p, -1);
    return finish(&p, syscall(SYS_fchownat, p.dir, p.name, uid, gid, flags));
}
int __wrap_chown(const char* path, uid_t uid, gid_t gid) { return __wrap_fchownat(AT_FDCWD, path, uid, gid, 0); }
int __wrap_lchown(const char* path, uid_t uid, gid_t gid) { return __wrap_fchownat(AT_FDCWD, path, uid, gid, AT_SYMLINK_NOFOLLOW); }
int __wrap_truncate(const char* path, off_t length) {
    int fd = __wrap_open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = ftruncate(fd, length), e = errno; close(fd); errno = e; return rc;
}
int __wrap_utimensat(int dir, const char* path, const struct timespec times[2], int flags) {
    int state = active(); if (state < 0) return -1;
    if (!state) return syscall(SYS_utimensat, dir, path, times, flags);
    // Bionic implements futimens through this syscall with a null pathname.
    // The descriptor already identifies the file; no path translation applies.
    if (!path) return syscall(SYS_utimensat, dir, NULL, times, flags);
    if (flags & ~AT_SYMLINK_NOFOLLOW) { errno = ENOTSUP; return -1; }
    Path p; if (resolve(dir, path, !(flags & AT_SYMLINK_NOFOLLOW), &p)) return -1;
    if (require_directory(&p)) return finish(&p, -1);
    return finish(&p, syscall(SYS_utimensat, p.dir, p.name, times, flags));
}

static int socket_address(const struct sockaddr* address, socklen_t length,
                          bool follow, struct sockaddr_un* mapped, socklen_t* mapped_length) {
    const size_t offset = offsetof(struct sockaddr_un, sun_path);
    if (length <= offset || length > sizeof(*mapped)) { errno = ENOTSUP; return -1; }
    const struct sockaddr_un* input = (const struct sockaddr_un*)address;
    size_t capacity = length - offset;
    if (!input->sun_path[0] || input->sun_path[0] != '/') {
        // Anonymous/abstract namespaces need a separate instance-scoping ABI.
        errno = ENOTSUP; return -1;
    }
    size_t size = strnlen(input->sun_path, capacity);
    if (size == capacity) { errno = ENAMETOOLONG; return -1; }
    Path p; if (resolve(AT_FDCWD, input->sun_path, follow, &p)) return -1;
    char path[PATH_MAX];
    int rc = require_directory(&p) || host_path(&p, path) ? -1 : 0;
    finish(&p, rc);
    if (rc) return -1;
    size = strlen(path);
    if (size >= sizeof(mapped->sun_path)) { errno = ENAMETOOLONG; return -1; }
    memset(mapped, 0, sizeof(*mapped)); mapped->sun_family = AF_UNIX;
    memcpy(mapped->sun_path, path, size + 1);
    *mapped_length = offset + size + 1;
    return 0;
}
static int socket_call(int nr, int fd, const struct sockaddr* address, socklen_t length, bool follow) {
    int state = active(); if (state < 0) return -1;
    if (!state || !address || length < sizeof(sa_family_t) || address->sa_family != AF_UNIX)
        return syscall(nr, fd, address, length);
    struct sockaddr_un mapped; socklen_t mapped_length;
    if (socket_address(address, length, follow, &mapped, &mapped_length)) return -1;
    int rc=syscall(nr,fd,&mapped,mapped_length);
    if (!rc && nr==SYS_bind) {
#ifdef POCKET_DIRECT_BOOT
        rc=pocket_boot_created_at(AT_FDCWD,mapped.sun_path);
#endif
    }
    return rc;
}
int __wrap_bind(int fd, const struct sockaddr* address, socklen_t length) {
    return socket_call(SYS_bind, fd, address, length, false);
}
int __wrap_connect(int fd, const struct sockaddr* address, socklen_t length) {
    return socket_call(SYS_connect, fd, address, length, true);
}
static int socket_name(int nr, int fd, struct sockaddr* address, socklen_t* length) {
    int state = active(); if (state < 0) return -1;
    if (!state || !length) return syscall(nr, fd, address, length);
    struct sockaddr_storage actual;
    socklen_t actual_length = sizeof(actual);
    if (syscall(nr, fd, &actual, &actual_length)) return -1;
    const size_t offset = offsetof(struct sockaddr_un, sun_path);
    if (actual_length > sizeof(actual)) { errno = ENOTSUP; return -1; }
    struct sockaddr_un guest;
    const void* output = &actual;
    socklen_t output_length = actual_length;
    if (actual.ss_family == AF_UNIX && actual_length > offset) {
        const struct sockaddr_un* unix_address = (const struct sockaddr_un*)&actual;
        if (unix_address->sun_path[0]) {
            char path[sizeof(guest.sun_path) + 1];
            size_t n = actual_length - offset;
            if (n > sizeof(guest.sun_path)) n = sizeof(guest.sun_path);
            memcpy(path, unix_address->sun_path, n); path[n] = 0;
            size_t prefix = strlen(root);
            if (strncmp(path, root, prefix) || path[prefix] != '/') { errno = EXDEV; return -1; }
            memset(&guest, 0, sizeof(guest)); guest.sun_family = AF_UNIX;
            strcpy(guest.sun_path, path + prefix);
            output = &guest; output_length = offset + strlen(guest.sun_path) + 1;
        }
    }
    socklen_t copy = *length < output_length ? *length : output_length;
    if (copy && !address) { errno = EFAULT; return -1; }
    if (copy) memcpy(address, output, copy);
    *length = output_length; return 0;
}
int __wrap_getsockname(int fd, struct sockaddr* address, socklen_t* length) { return socket_name(SYS_getsockname, fd, address, length); }
int __wrap_getpeername(int fd, struct sockaddr* address, socklen_t* length) { return socket_name(SYS_getpeername, fd, address, length); }

// The loader runs before our libc hooks. Translate an executable's embedded
// library search path for it, without modifying the program or its packages.
static char* executable_runpath(int fd) {
    Elf64_Ehdr elf;
    if (pread(fd, &elf, sizeof(elf), 0) != sizeof(elf) ||
        memcmp(elf.e_ident, ELFMAG, SELFMAG) || elf.e_ident[EI_CLASS] != ELFCLASS64 ||
        elf.e_ident[EI_DATA] != ELFDATA2LSB || elf.e_phentsize != sizeof(Elf64_Phdr) ||
        !elf.e_phnum || elf.e_phnum > 1024 || elf.e_phoff > INT64_MAX) return NULL;
    size_t bytes = elf.e_phnum * sizeof(Elf64_Phdr);
    Elf64_Phdr* segments = malloc(bytes);
    if (!segments) return NULL;
    if (pread(fd, segments, bytes, elf.e_phoff) != (ssize_t)bytes) { free(segments); return NULL; }
    uint64_t table = 0, size = 0, runpath = UINT64_MAX, rpath = UINT64_MAX;
    for (unsigned i = 0; i < elf.e_phnum; ++i) {
        Elf64_Phdr* p = &segments[i];
        if (p->p_type != PT_DYNAMIC || p->p_filesz > 65536 || p->p_offset > INT64_MAX-65536) continue;
        for (uint64_t offset = 0; offset+sizeof(Elf64_Dyn) <= p->p_filesz; offset += sizeof(Elf64_Dyn)) {
            Elf64_Dyn item;
            if (pread(fd, &item, sizeof(item), p->p_offset+offset) != sizeof(item) || item.d_tag == DT_NULL) break;
            if (item.d_tag == DT_STRTAB) table = item.d_un.d_ptr;
            if (item.d_tag == DT_STRSZ) size = item.d_un.d_val;
            if (item.d_tag == DT_RUNPATH) runpath = item.d_un.d_val;
            if (item.d_tag == DT_RPATH) rpath = item.d_un.d_val;
        }
    }
    if (runpath == UINT64_MAX) runpath = rpath;
    char* value = NULL;
    for (unsigned i = 0; table && runpath < size && i < elf.e_phnum; ++i) {
        Elf64_Phdr* p = &segments[i];
        if (p->p_type != PT_LOAD || table < p->p_vaddr || table-p->p_vaddr >= p->p_filesz) continue;
        uint64_t delta = table-p->p_vaddr;
        if (runpath >= p->p_filesz-delta || p->p_filesz > INT64_MAX || p->p_offset > INT64_MAX-p->p_filesz) break;
        size_t length = size-runpath > PATH_MAX ? PATH_MAX : (size_t)(size-runpath);
        if (length > p->p_filesz-delta-runpath) length = p->p_filesz-delta-runpath;
        value = malloc(length);
        if (value && (pread(fd, value, length, p->p_offset+delta+runpath) != (ssize_t)length || !memchr(value, 0, length))) {
            free(value); value = NULL;
        }
        break;
    }
    free(segments); return value;
}
static int append_exec_paths(char** paths, const char* search, const char* executable) {
    if (!search) return 0;
    char* copy = strdup(search);
    if (!copy) return -1;
    char* remaining = copy;
    char* part;
    while ((part = strsep(&remaining, ":"))) {
        char expanded[PATH_MAX], mapped[PATH_MAX];
        const char* path = *part ? part : ".";
        size_t token = !strncmp(path, "$ORIGIN", 7) ? 7 : !strncmp(path, "${ORIGIN}", 9) ? 9 : 0;
        if (token) {
            const char* slash = strrchr(executable, '/');
            if (!slash || snprintf(expanded, sizeof(expanded), "%.*s%s", (int)(slash-executable), executable, path+token) >= (int)sizeof(expanded)) {
                free(copy); errno = ENAMETOOLONG; return -1;
            }
            path = expanded;
        }
        size_t prefix = strlen(root);
        if (!strncmp(path, root, prefix) && (path[prefix] == '/' || !path[prefix]))
            path = path[prefix] ? path+prefix : "/";
        Path directory;
        if (resolve(AT_FDCWD, path, true, &directory)) continue;
        int rc = host_path(&directory, mapped); finish(&directory, rc);
        if (rc) { free(copy); return -1; }
        bool present = false;
        size_t length = strlen(mapped);
        for (const char* at = *paths; *at;) {
            const char* end = strchr(at, ':');
            size_t size = end ? (size_t)(end-at) : strlen(at);
            if (size == length && !memcmp(at, mapped, length)) { present = true; break; }
            if (!end) break;
            at = end+1;
        }
        if (present) continue;
        char* joined;
        if (asprintf(&joined, "%s:%s", *paths, mapped) < 0) { free(copy); return -1; }
        free(*paths); *paths = joined;
    }
    free(copy); return 0;
}

#ifdef POCKET_DIRECT_BOOT
typedef struct {
    uint64_t magic, device, inode, size;
    int64_t modified, modified_ns, changed, changed_ns;
} ExecStamp;
static ExecStamp exec_stamp(const struct stat* st) {
    return (ExecStamp){0x504f434b45584531ULL, st->st_dev, st->st_ino, st->st_size,
        st->st_mtim.tv_sec, st->st_mtim.tv_nsec, st->st_ctim.tv_sec, st->st_ctim.tv_nsec};
}
static int write_exec_at(int fd, const void* bytes, size_t size, off_t offset) {
    while (size) {
        ssize_t count = pwrite(fd, bytes, size, offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (!count) errno = EIO; return -1; }
        size -= count; bytes = (const char*)bytes+count; offset += count;
    }
    return 0;
}
// Downloaded Android executables still name /system/bin/linker64. Keep the
// original intact and atomically cache a copy beside it, with a private guest
// PT_INTERP. Keeping the same directory preserves $ORIGIN library resolution.
static int prepare_exec_interpreter(Path* path, int fd, char* executable) {
    Elf64_Ehdr elf;
    if (pread(fd, &elf, sizeof(elf), 0) != sizeof(elf) ||
        memcmp(elf.e_ident, ELFMAG, SELFMAG) || elf.e_ident[EI_CLASS] != ELFCLASS64 ||
        elf.e_ident[EI_DATA] != ELFDATA2LSB || elf.e_phentsize != sizeof(Elf64_Phdr) ||
        elf.e_phnum > 1024 || elf.e_phoff > INT64_MAX-65536) return 0;
    for (unsigned i = 0; i < elf.e_phnum; ++i) {
        off_t position = elf.e_phoff+i*sizeof(Elf64_Phdr);
        Elf64_Phdr program;
        if (pread(fd, &program, sizeof(program), position) != sizeof(program)) return 0;
        if (program.p_type != PT_INTERP) continue;
        char original[PATH_MAX], linker[PATH_MAX];
        if (!program.p_filesz || program.p_filesz > sizeof(original) || program.p_offset > INT64_MAX-PATH_MAX ||
            pread(fd, original, program.p_filesz, program.p_offset) != (ssize_t)program.p_filesz ||
            original[program.p_filesz-1] || strcmp(original, "/system/bin/linker64")) return 0;
        if (snprintf(linker, sizeof(linker), "%s/system/bin/linker64", root) >= (int)sizeof(linker)) { errno = ENAMETOOLONG; return -1; }
        if (syscall(SYS_faccessat, path->dir, path->name, X_OK)) return -1;
        struct stat source;
        if (syscall(SYS_fstat, fd, &source)) return -1;
        if (source.st_size < 0 || source.st_size > INT64_MAX-PATH_MAX-(off_t)sizeof(ExecStamp)) { errno = EFBIG; return -1; }
        ExecStamp stamp = exec_stamp(&source);
        size_t length = strlen(linker)+1;
        uint64_t hash = 14695981039346656037ULL;
        for (const unsigned char* c = (const unsigned char*)path->name; *c; ++c) hash = (hash^*c)*1099511628211ULL;
        char cache[96], temporary[128];
        snprintf(cache, sizeof(cache), ".pocket-exec-%016llx", (unsigned long long)hash);
        int old = raw_open(path->dir, cache, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0);
        bool valid = false;
        if (old >= 0) {
            struct stat stored; ExecStamp saved; char saved_linker[PATH_MAX];
            valid = !syscall(SYS_fstat, old, &stored) && S_ISREG(stored.st_mode) && (stored.st_mode & S_IXUSR) &&
                stored.st_size == source.st_size+(off_t)length+(off_t)sizeof(stamp) &&
                pread(old, &saved, sizeof(saved), source.st_size+length) == sizeof(saved) && !memcmp(&stamp, &saved, sizeof(stamp)) &&
                pread(old, saved_linker, length, source.st_size) == (ssize_t)length && !memcmp(linker, saved_linker, length);
            close(old);
        }
        if (!valid) {
            snprintf(temporary, sizeof(temporary), "%s.%d.%ld.tmp", cache, getpid(), syscall(SYS_gettid));
            int copy = raw_open(path->dir, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0700);
            if (copy < 0) return -1;
            char buffer[65536]; off_t offset = 0;
            int rc = syscall(SYS_fchmod, copy, 0700);
            while (!rc && offset < source.st_size) {
                size_t count = source.st_size-offset > (off_t)sizeof(buffer) ? sizeof(buffer) : (size_t)(source.st_size-offset);
                ssize_t read_count = pread(fd, buffer, count, offset);
                if (read_count < 0 && errno == EINTR) continue;
                if (read_count <= 0) { if (!read_count) errno = EIO; rc = -1; break; }
                if (write_exec_at(copy, buffer, read_count, offset)) { rc = -1; break; }
                offset += read_count;
            }
            program.p_offset = source.st_size; program.p_vaddr = program.p_paddr = 0;
            program.p_filesz = program.p_memsz = length;
            if (!rc) rc = write_exec_at(copy, linker, length, source.st_size) ||
                write_exec_at(copy, &stamp, sizeof(stamp), source.st_size+length) ||
                write_exec_at(copy, &program, sizeof(program), position);
            struct stat after;
            if (!rc) {
                rc = syscall(SYS_fstat, fd, &after);
                if (!rc) { ExecStamp current = exec_stamp(&after); if (memcmp(&stamp, &current, sizeof(stamp))) { errno = EAGAIN; rc = -1; } }
            }
            int saved = errno;
            if (close(copy) && !rc) { rc = -1; saved = errno; }
            if (!rc && syscall(SYS_renameat, path->dir, temporary, path->dir, cache)) { rc = -1; saved = errno; }
            syscall(SYS_unlinkat, path->dir, temporary, 0);
            if (rc) { errno = saved; return -1; }
        }
        Path cached = *path; strcpy(cached.name, cache);
        return host_path(&cached, executable);
    }
    return 0;
}
#endif

static int exec_guest(const char* file, char* const argv[], char* const envp[], unsigned depth) {
    int state = active(); if (state < 0) return -1;
    if (!state) return syscall(SYS_execve, file, argv, envp);
    Path p; if (resolve(AT_FDCWD, file, true, &p)) return -1;
    char executable[PATH_MAX];
    if (require_directory(&p) || host_path(&p, executable)) return finish(&p, -1);
    // The kernel cannot resolve a guest-only shebang interpreter. Dispatch it
    // through the same guest path resolver, retaining Linux's single optional
    // argument and the caller's script name instead of invoking a host shell.
    const char* libraries = getenv("POCKET_DIRECT_LIBRARIES");
#ifdef POCKET_DIRECT_BOOT
    char pinned_libraries[PATH_MAX*2+32];
    const char* view=module_view_ready()?"/.pocket-module-view":"";
    if (snprintf(pinned_libraries, sizeof(pinned_libraries), "%s%s/system/lib64:%s%s/vendor/lib64", root,view,root,view) >= (int)sizeof(pinned_libraries)) {
        close(p.dir); errno = ENAMETOOLONG; return -1;
    }
    libraries = pinned_libraries;
#endif
    char* runpath = NULL;
    int fd = raw_open(p.dir, p.name, O_RDONLY | O_NONBLOCK | O_CLOEXEC, 0);
    if (fd >= 0) {
        char header[257];
        ssize_t count;
        do { count = read(fd, header, sizeof(header)-1); } while (count < 0 && errno == EINTR);
        if (count >= 2 && header[0] == '#' && header[1] == '!') {
            close(fd);
            if (syscall(SYS_faccessat, p.dir, p.name, X_OK)) return finish(&p, -1);
            close(p.dir);
            if (depth >= 4) { errno = ELOOP; return -1; }
            header[count] = 0;
            char* end = memchr(header, '\n', count);
            if (!end) {
                end = memchr(header, 0, count);
                if (!end) {
                    // Never execute a truncated interpreter pathname.
                    end = header + count;
                    if (count == (ssize_t)sizeof(header)-1) { errno = ENOEXEC; return -1; }
                }
            }
            while (end > header+2 && (end[-1] == ' ' || end[-1] == '\t')) --end;
            *end = 0;
            char* interpreter = header+2;
            while (*interpreter == ' ' || *interpreter == '\t') ++interpreter;
            if (!*interpreter) { errno = ENOEXEC; return -1; }
            char* argument = interpreter;
            while (*argument && *argument != ' ' && *argument != '\t') ++argument;
            if (*argument) *argument++ = 0;
            while (*argument == ' ' || *argument == '\t') ++argument;
            size_t argc = 0;
            while (argv && argv[argc]) ++argc;
            if (argc > SIZE_MAX/sizeof(char*)-4) { errno = E2BIG; return -1; }
            char** args = calloc(argc+4, sizeof(char*));
            if (!args) { errno = ENOMEM; return -1; }
            size_t used = 0;
            args[used++] = interpreter;
            if (*argument) args[used++] = argument;
            args[used++] = (char*)file;
            for (size_t i = 1; i < argc; ++i) args[used++] = argv[i];
            int rc = exec_guest(interpreter, args, envp, depth+1);
            int saved = errno;
            free(args); errno = saved; return rc;
        }
        if (libraries) runpath = executable_runpath(fd);
#ifdef POCKET_DIRECT_BOOT
        if (prepare_exec_interpreter(&p, fd, executable)) {
            int saved = errno; close(fd); free(runpath); errno = saved; return finish(&p, -1);
        }
#endif
        close(fd);
    }
    close(p.dir);
    // Android fixtures have their ELF interpreter set to the pinned guest
    // linker. Preserve its library path when the caller supplies a fresh envp.
    size_t envc = 0;
    while (envp && envp[envc]) ++envc;
    char* search = libraries ? strdup(libraries) : NULL;
    if (libraries && !search) { free(runpath); return -1; }
    for (size_t i = 0; libraries && i < envc; ++i)
        if (!strncmp(envp[i], "LD_LIBRARY_PATH=", 16) && append_exec_paths(&search, envp[i]+16, executable)) {
            free(search); free(runpath); return -1;
        }
    if (libraries && append_exec_paths(&search, runpath, executable)) { free(search); free(runpath); return -1; }
    free(runpath);
    char** env = calloc(envc + 24, sizeof(char*));
    char *rootenv = NULL, *searchenv = NULL, *libenv = NULL, *configenv = NULL;
    if (!env || asprintf(&rootenv, "POCKET_DIRECT_ROOT=%s", root) < 0 ||
        (libraries && (asprintf(&searchenv, "LD_LIBRARY_PATH=%s", search) < 0 ||
                       asprintf(&libenv, "POCKET_DIRECT_LIBRARIES=%s", libraries) < 0 ||
                       asprintf(&configenv, "LD_CONFIG_FILE=%s/system/etc/ld.config.txt", root) < 0))) {
        free(search); free(env); free(rootenv); free(searchenv); free(libenv); free(configenv); errno = ENOMEM; return -1;
    }
    free(search);
    size_t j = 0;
    for (size_t i = 0; i < envc; ++i)
        if (strncmp(envp[i], "POCKET_DIRECT_ROOT=", 19) &&
            strncmp(envp[i], "POCKET_DIRECT_LIBRARIES=", 24) &&
            (!libraries || (strncmp(envp[i], "LD_LIBRARY_PATH=", 16) && strncmp(envp[i], "LD_CONFIG_FILE=", 15)))) env[j++] = envp[i];
    env[j++] = rootenv;
    if (libraries) { env[j++] = searchenv; env[j++] = libenv; env[j++] = configenv; }
#ifdef POCKET_DIRECT_BOOT
    const char* keys[] = {"POCKET_DIRECT_ID", "POCKET_DIRECT_GROUPS", "POCKET_DIRECT_CONSOLE", "POCKET_PARENT_PID", NULL};
    char* extra[8] = {0}; int extras = 0;
    for (int k = 0; keys[k]; ++k) {
        char value[PATH_MAX];
        int found = pocket_boot_exec_value(keys[k], value, sizeof(value));
        if (found < 0) _exit(126);
        if (!found) continue;
        size_t key_length = strlen(keys[k]);
        for (size_t i = 0; i < j;)
            if (!strncmp(env[i], keys[k], key_length) && env[i][key_length] == '=') env[i] = env[--j]; else ++i;
        if (asprintf(&extra[extras], "%s=%s", keys[k], value) < 0) _exit(126);
        env[j++] = extra[extras++];
    }
    // Always retain the runtime ahead of any RC-provided guest preload.
    char preload[PATH_MAX * 4 + 256];
    snprintf(preload, sizeof(preload), "LD_PRELOAD=%s/system/lib64/libpocket_direct_boot.so:%s/system/lib64/libpocket_binder_api28.so:%s/system/lib64/libpocket_hidl_api28.so", root, root, root);
    // Explicit owner opt-in, restricted to the guest keystore executable.
    // Other guest/host processes retain the normal preload list.
    char keystore_path[PATH_MAX], config_path[PATH_MAX], attestation_library[PATH_MAX];
    snprintf(keystore_path, sizeof(keystore_path), "%s/system/bin/keystore", root);
    snprintf(config_path, sizeof(config_path), "%s/%s/misc/keystore/pocket-teesim.json", root, data_alias);
    snprintf(attestation_library, sizeof(attestation_library), "%s/system/lib64/libpocket_teesim_api28.so", root);
    struct stat config_stat, library_stat;
    if (!strcmp(executable, keystore_path) && data_alias[0] &&
        raw_stat(AT_FDCWD, config_path, &config_stat, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(config_stat.st_mode) && (config_stat.st_mode & 0077) == 0 &&
        raw_stat(AT_FDCWD, attestation_library, &library_stat, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(library_stat.st_mode)) {
        size_t used = strlen(preload);
        snprintf(preload + used, sizeof(preload) - used, ":%s/system/lib64/libpocket_teesim_api28.so", root);
    }
    for (size_t i = 0; i < j;) if (!strncmp(env[i], "LD_PRELOAD=", 11)) env[i] = env[--j]; else ++i;
    env[j++] = preload;
    env[j] = NULL;
#endif
    int rc = syscall(SYS_execve, executable, argv, env);
    int e = errno;
    free(env); free(rootenv); free(searchenv); free(libenv); free(configenv);
#ifdef POCKET_DIRECT_BOOT
    for (int i = 0; i < extras; ++i) free(extra[i]);
#endif
    errno = e;
    return rc;
}
int __wrap_execve(const char* file, char* const argv[], char* const envp[]) {
    return exec_guest(file, argv, envp, 0);
}
