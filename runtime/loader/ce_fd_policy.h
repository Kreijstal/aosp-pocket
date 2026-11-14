// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// O_PATH is metadata authority. Data, directory iteration, mutation and locks
// must retain the kernel's EBADF semantics even when CE emulates an operation.
static int ce_path_only_operation(long nr,long command) {
    switch(nr) {
    case __NR_fstat:case __NR_fstatfs:case __NR_close:case __NR_dup:case __NR_dup3:case __NR_fchdir:return 1;
    case __NR_fcntl:return command==F_GETFL || command==F_GETFD || command==F_SETFD || command==F_DUPFD || command==F_DUPFD_CLOEXEC;
    default:return 0;
    }
}
