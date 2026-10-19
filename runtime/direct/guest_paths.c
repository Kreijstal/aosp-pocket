// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include "guest_paths.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#if defined(SYS_newfstatat)
#define POCKET_FSTATAT SYS_newfstatat
#else
#define POCKET_FSTATAT SYS_fstatat64
#endif

static pthread_once_t once = PTHREAD_ONCE_INIT;
static char root[PATH_MAX];
static int root_fd = -1, init_error;
static bool enabled;

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
    root_fd = raw_open(AT_FDCWD, value, O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
    if (root_fd < 0) { init_error = errno; return; }
    // Do not call wrapped realpath during pthread_once initialization.
    if (fd_path(root_fd, root) || !strcmp(root, "/")) {
        init_error = errno ? errno : EINVAL;
        close(root_fd); root_fd = -1;
    }
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
        errno = EXDEV; return -1;
    }
    strcpy(guest, actual[n] ? actual + n : "/");
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
    char todo[PATH_MAX], base[PATH_MAX];
    if (*path == '/') strcpy(todo, path);
    else {
        if (directory_name(dir, base)) return -1;
        if (snprintf(todo, sizeof(todo), "%s/%s", base, path) >= (int)sizeof(todo)) {
            errno = ENAMETOOLONG; return -1;
        }
    }
    int stack[PATH_MAX / 2], depth = 0, links = 0;
    stack[0] = fcntl(root_fd, F_DUPFD_CLOEXEC, 3);
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

int __wrap_openat(int dir, const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args; va_start(args, flags); mode = (mode_t)va_arg(args, int); va_end(args);
    }
    int state = active(); if (state < 0) return -1;
    if (!state) return raw_open(dir, path, flags, mode);
    if ((flags & O_TMPFILE) == O_TMPFILE) { errno = ENOTSUP; return -1; }
    Path p;
    if (resolve(dir, path, !(flags & O_NOFOLLOW) && !((flags & O_CREAT) && (flags & O_EXCL)), &p)) return -1;
    return finish(&p, raw_open(p.dir, p.name, flags | (p.slash ? O_DIRECTORY : 0), mode));
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
ssize_t __wrap_readlinkat(int dir, const char* path, char* out, size_t size) {
    int state = active(); if (state < 0) return -1;
    if (!state) return syscall(SYS_readlinkat, dir, path, out, size);
    Path p; if (resolve(dir, path, false, &p)) return -1;
    ssize_t rc = syscall(SYS_readlinkat, p.dir, p.name, out, size);
    int e = errno; close(p.dir); errno = e; return rc;
}
ssize_t __wrap_readlink(const char* path, char* out, size_t size) { return __wrap_readlinkat(AT_FDCWD, path, out, size); }
int __wrap_mkdirat(int dir, const char* path, mode_t mode) {
    int state = active(); if (state < 0) return -1;
    if (!state) return syscall(SYS_mkdirat, dir, path, mode);
    Path p; if (resolve(dir, path, false, &p)) return -1;
    return finish(&p, syscall(SYS_mkdirat, p.dir, p.name, mode));
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
    return finish(&p, syscall(SYS_symlinkat, target, p.dir, p.name));
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
    return syscall(nr, fd, &mapped, mapped_length);
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

int __wrap_execve(const char* file, char* const argv[], char* const envp[]) {
    int state = active(); if (state < 0) return -1;
    if (!state) return syscall(SYS_execve, file, argv, envp);
    Path p; if (resolve(AT_FDCWD, file, true, &p)) return -1;
    char executable[PATH_MAX];
    if (require_directory(&p) || host_path(&p, executable)) return finish(&p, -1);
    close(p.dir);
    // Android fixtures have their ELF interpreter set to the pinned guest
    // linker. Preserve its library path when the caller supplies a fresh envp.
    size_t envc = 0;
    while (envp && envp[envc]) ++envc;
    const char* libraries = getenv("POCKET_DIRECT_LIBRARIES");
    char** env = calloc(envc + 5, sizeof(char*));
    char *rootenv = NULL, *searchenv = NULL, *libenv = NULL, *configenv = NULL;
    if (!env || asprintf(&rootenv, "POCKET_DIRECT_ROOT=%s", root) < 0 ||
        (libraries && (asprintf(&searchenv, "LD_LIBRARY_PATH=%s", libraries) < 0 ||
                       asprintf(&libenv, "POCKET_DIRECT_LIBRARIES=%s", libraries) < 0 ||
                       asprintf(&configenv, "LD_CONFIG_FILE=%s/system/etc/ld.config.txt", root) < 0))) {
        free(env); free(rootenv); free(searchenv); free(libenv); free(configenv); errno = ENOMEM; return -1;
    }
    size_t j = 0;
    for (size_t i = 0; i < envc; ++i)
        if (strncmp(envp[i], "POCKET_DIRECT_ROOT=", 19) &&
            strncmp(envp[i], "POCKET_DIRECT_LIBRARIES=", 24) &&
            (!libraries || (strncmp(envp[i], "LD_LIBRARY_PATH=", 16) && strncmp(envp[i], "LD_CONFIG_FILE=", 15)))) env[j++] = envp[i];
    env[j++] = rootenv;
    if (libraries) { env[j++] = searchenv; env[j++] = libenv; env[j++] = configenv; }
    int rc = syscall(SYS_execve, executable, argv, env);
    int e = errno;
    free(env); free(rootenv); free(searchenv); free(libenv); free(configenv); errno = e;
    return rc;
}
