// SPDX-License-Identifier: GPL-3.0-only
#if !defined(__aarch64__)
#error This experimental syscall gate supports AArch64 only
#endif
.text
.p2align 2
.global pocket_profile_syscall
.hidden pocket_profile_syscall
.type pocket_profile_syscall,%function
pocket_profile_syscall:
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    mov x4, x5
    mov x5, x6
    svc #0
.global pocket_profile_syscall_return
.hidden pocket_profile_syscall_return
pocket_profile_syscall_return:
    ret
.size pocket_profile_syscall, .-pocket_profile_syscall
.section .note.GNU-stack,"",%progbits
