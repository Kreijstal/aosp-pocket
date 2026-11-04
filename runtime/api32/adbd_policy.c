// SPDX-License-Identifier: GPL-3.0-only
// Rootless 12L adbd: stock adbd drops to the shell role through minijail,
// which calls setgroups(). Android's app seccomp policy kills the outer app
// UID on that syscall, and the loader only virtualizes the uid/gid calls.
// This preload records the requested supplementary groups as guest
// credentials through the loader's group control call, the same one Pocket's
// init uses for service "group" lines. Kernel groups are not changed; the
// outer app UID remains the host security boundary.
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))
#define POCKET_CONTROL 0x504c2001UL
#define POCKET_SET_GROUPS 1UL
#define POCKET_GROUP_LIMIT 64

EXPORT int setgroups(size_t count, const gid_t* groups) {
    if (count > POCKET_GROUP_LIMIT || (count && !groups)) {
        errno = EINVAL;
        return -1;
    }
    long rc = syscall(__NR_getpid, POCKET_CONTROL, POCKET_SET_GROUPS, (unsigned long)count, groups);
    if (rc < 0) {
        // Raw syscall() already set errno from the loader's result.
        fprintf(stderr, "POCKET_ADBD_SETGROUPS_FAILED count=%zu errno=%d\n", count, errno);
        return -1;
    }
    if (rc > 0) {
        errno = (int)rc;
        return -1;
    }
    return 0;
}

// adbd's server_socket_thread() inspects errno after a successful listen
// helper and abandons the listener on a stale EINVAL. In the guest, errno
// already holds EINVAL from an earlier call on that thread, so the loopback
// listener was closed immediately after binding. listen() is not loader
// virtualized; issue it directly and clear errno on success.
EXPORT int listen(int fd, int backlog) {
    long rc = syscall(__NR_listen, fd, backlog);
    if (rc == 0) errno = 0;
    return (int)rc;
}
