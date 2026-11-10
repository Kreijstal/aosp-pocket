// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>

// Stock MediaProvider expects private emulated directories to be bind-mounted
// outside FUSE. In Pocket's explicitly selected backend they stay in FUSE and
// must use MediaProvider's existing package/UID policy instead. Never enable
// this adaptation on a normal Android process or an unselected guest image.
static bool pocket_private_fuse_enabled() {
    static const bool enabled=[] {
        struct stat state{};
        bool selected=syscall(__NR_getpid,0x504c1001UL)==0x504c1002UL
            && syscall(__NR_getpid,0x504c2001UL,7UL)==3
            && !lstat("/system/etc/pocket-external-volume32",&state) && S_ISREG(state.st_mode);
        if(selected)fputs("POCKET_MEDIA_PRIVATE_FUSE_POLICY enabled=1 owner_checks=MediaProvider kernel_bind_mounts=none\n",stderr);
        return selected;
    }();
    return enabled;
}
