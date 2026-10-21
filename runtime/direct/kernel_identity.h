// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdio.h>
#include <sys/types.h>

// Host Binder/SO_PEERCRED credentials remain real kernel identities even when
// a syscall backend virtualizes getuid(). Fail closed if proc is unavailable.
static inline uid_t pocket_kernel_uid_from_proc(void) {
    FILE* file=fopen("/proc/self/status","re");
    if(!file)return (uid_t)-1;
    char line[256];unsigned uid=(uid_t)-1;
    while(fgets(line,sizeof(line),file))if(sscanf(line,"Uid: %u",&uid)==1)break;
    fclose(file);return uid;
}
