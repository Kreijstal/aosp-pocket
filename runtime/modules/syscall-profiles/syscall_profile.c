// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include "syscall_profile.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>

#if !defined(__aarch64__)
#error This experimental profile supports AArch64 only
#endif

extern long pocket_profile_syscall(long nr, long a0, long a1, long a2,
                                   long a3, long a4, long a5);
extern const char pocket_profile_syscall_return[];
static char guest[PATH_MAX], host[PATH_MAX];
static size_t guest_size, host_size;
static int installed;
static struct sigaction prior_handler;
enum { PROFILE_MARKER = 0x504b };

static long raw(long nr, long a0, long a1, long a2, long a3) {
    return pocket_profile_syscall(nr, a0, a1, a2, a3, 0, 0);
}

/* Kernel-mediated copy handles invalid app pointers without faulting in the
 * signal handler. Open self/mem afresh: a descriptor retained across fork
 * would still refer to the parent's address space. No allocation or locks.
 */
static long copy_path(char *buffer, uintptr_t pointer) {
    long fd = raw(__NR_openat, AT_FDCWD, (long)"/proc/self/mem", O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return fd;
    long count = raw(__NR_pread64, fd, (long)buffer, PATH_MAX, (long)pointer);
    raw(__NR_close, fd, 0, 0, 0);
    if (count <= 0) return -EFAULT;
    for (long i = 0; i < count; i++) if (!buffer[i]) return i;
    return count == PATH_MAX ? -ENAMETOOLONG : -EFAULT;
}

static long adapt_openat(const mcontext_t *context) {
    char path[PATH_MAX], mapped[PATH_MAX];
    long count = copy_path(path, context->regs[1]);
    if (count < 0) return count;
    size_t length = (size_t)count;
    int match = length >= guest_size;
    for (size_t i = 0; match && i < guest_size; i++) match = path[i] == guest[i];
    match = match && (!path[guest_size] || path[guest_size] == '/');
    const char *target = path;
    if (match) {
        /* Reject lexical escapes. Symlink containment is not provided. */
        for (size_t i = guest_size; i < length; i++)
            if (path[i] == '/' && path[i+1] == '.' && path[i+2] == '.' &&
                (!path[i+3] || path[i+3] == '/')) return -EPERM;
        size_t suffix = length - guest_size;
        if (host_size + suffix >= sizeof(mapped)) return -ENAMETOOLONG;
        for (size_t i = 0; i < host_size; i++) mapped[i] = host[i];
        for (size_t i = 0; i <= suffix; i++) mapped[host_size+i] = path[guest_size+i];
        target = mapped;
    }
    return raw(__NR_openat, (long)context->regs[0], (long)target,
               (long)context->regs[2], (long)context->regs[3]);
}

static void trapped(int signal, siginfo_t *info, void *opaque) {
    (void)signal;
    int saved_errno = errno;
    ucontext_t *context = opaque;
    if (info->si_code != SYS_SECCOMP || info->si_errno != PROFILE_MARKER ||
        info->si_arch != AUDIT_ARCH_AARCH64 || info->si_syscall != __NR_openat) {
        /* Preserve the existing runtime's handling of unrelated traps. */
        if (prior_handler.sa_handler != SIG_DFL && prior_handler.sa_handler != SIG_IGN) {
            if (prior_handler.sa_flags & SA_SIGINFO) prior_handler.sa_sigaction(signal, info, opaque);
            else prior_handler.sa_handler(signal);
            errno = saved_errno;
        } else raw(__NR_exit_group, 159, 0, 0, 0);
        return;
    }
    context->uc_mcontext.regs[0] = (uint64_t)adapt_openat(&context->uc_mcontext);
    errno = saved_errno;
}

int pocket_syscall_profile_install(enum pocket_syscall_profile profile,
                                  const char *guest_prefix, const char *host_prefix) {
    if (installed) {errno = EALREADY; return -1;}
    if (profile == POCKET_SYSCALL_OFF) return 0;
    if ((profile != POCKET_SYSCALL_OPENAT_PREFIX && profile != POCKET_SYSCALL_OPENAT_PREFIX_CHAINED) || !guest_prefix || !host_prefix ||
        guest_prefix[0] != '/' || host_prefix[0] != '/') {errno = EINVAL; return -1;}
    size_t gs = strnlen(guest_prefix, PATH_MAX), hs = strnlen(host_prefix, PATH_MAX);
    if (gs < 2 || hs < 2 || gs == PATH_MAX || hs == PATH_MAX ||
        guest_prefix[gs-1] == '/' || host_prefix[hs-1] == '/') {errno = EINVAL; return -1;}
    struct sigaction previous, action = {0};
    if (sigaction(SIGSYS, NULL, &previous)) return -1;
    if ((profile != POCKET_SYSCALL_OPENAT_PREFIX_CHAINED && previous.sa_handler != SIG_DFL) ||
        previous.sa_handler == SIG_IGN || (previous.sa_flags & SA_RESETHAND)) {errno = EBUSY; return -1;}
    prior_handler = previous;
    memcpy(guest, guest_prefix, gs+1); memcpy(host, host_prefix, hs+1);
    guest_size = gs; host_size = hs;
    action.sa_sigaction = trapped;
    action.sa_flags = SA_SIGINFO | (previous.sa_flags & (SA_ONSTACK | SA_RESTART | SA_NODEFER));
    action.sa_mask = previous.sa_mask;
    if (sigaction(SIGSYS, &action, NULL)) return -1;
    uintptr_t gate = (uintptr_t)pocket_profile_syscall_return;
    struct sock_filter instructions[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_execve, 1, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_execveat, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EOPNOTSUPP),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_openat, 0, 5),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, instruction_pointer)+4),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)(gate >> 32), 0, 2),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, instruction_pointer)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)gate, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP | PROFILE_MARKER),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = {.len = sizeof(instructions)/sizeof(instructions[0]), .filter = instructions};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
        prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program)) {
        int error = errno;
        sigaction(SIGSYS, &previous, NULL);
        errno = error;
        return -1;
    }
    installed = 1;
    return 0;
}
