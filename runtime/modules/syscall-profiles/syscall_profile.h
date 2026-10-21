// SPDX-License-Identifier: GPL-3.0-only
#pragma once

enum pocket_syscall_profile {
    POCKET_SYSCALL_OFF = 0,
    POCKET_SYSCALL_OPENAT_PREFIX = 1,
    POCKET_SYSCALL_OPENAT_PREFIX_CHAINED = 2,
};
#ifdef __cplusplus
extern "C" {
#endif

/* Experimental compatibility adapter, NOT a filesystem sandbox.
 * Call once in a single-threaded child, before app code and worker threads.
 * Only absolute openat paths under guest_prefix are translated. Everything
 * else passes through. Filters survive fork and cannot be removed; do not
 * exec after installation (the signal handler would no longer exist).
 * The selected profile rejects execve/execveat with EOPNOTSUPP for this reason.
 * A separately launched process is required to select another profile.
 */
int pocket_syscall_profile_install(enum pocket_syscall_profile profile,
                                  const char *guest_prefix, const char *host_prefix);
#ifdef __cplusplus
}
#endif
