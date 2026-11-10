// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/types.h>
// Explicit guest-only API for platform code. Android's host app filter forbids
// setgroups, so source-built zygote requests logical groups using an allowed
// syscall with a reserved selector. Stock Bionic and host groups stay unchanged.
static inline int pocket_native_setgroups(size_t count,const gid_t* groups) {
    return (int)syscall(__NR_getpid,0x504c2001UL,1UL,count,groups);
}
// Scope only the host bootstrap transactions used to obtain the private guest
// registry. Ordinary app context-manager lookups must stay in the guest.
static inline void pocket_native_binder_host(int enabled) {
    (void)syscall(__NR_getpid,0x504c2001UL,2UL,(unsigned long)enabled);
}
// Monotonic per-process view selection, requested before zygote drops UID 0.
static inline int pocket_native_denylist_view(void) {
    return (int)syscall(__NR_getpid,0x504c2001UL,3UL);
}
// Preserve the framework's external-storage view without kernel namespaces.
// Only a logical UID0 caller (zygote before specialization) can select it.
static inline int pocket_native_storage_mode(unsigned mode) {
    return (int)syscall(__NR_getpid,0x504c2001UL,6UL,(unsigned long)mode);
}
