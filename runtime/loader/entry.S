// SPDX-License-Identifier: GPL-3.0-only
#if !defined(__aarch64__)
#error AArch64 only
#endif
.text
.p2align 2
.global pocket_entry
.type pocket_entry,%function
pocket_entry:
    mov x19, sp
    mov x0, sp
    bl pocket_loader_start
    mov sp, x19
    br x0
.size pocket_entry, .-pocket_entry

.global pocket_raw_syscall
.hidden pocket_raw_syscall
.type pocket_raw_syscall,%function
pocket_raw_syscall:
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    mov x4, x5
    mov x5, x6
    svc #0
    ret
.size pocket_raw_syscall, .-pocket_raw_syscall

.global pocket_restore_rt
.hidden pocket_restore_rt
.type pocket_restore_rt,%function
pocket_restore_rt:
    mov x8, #139
    svc #0
    brk #0
.size pocket_restore_rt, .-pocket_restore_rt

// Outside the loader's syscall bypass window. It detects the inherited filter
// after the new handler is installed, avoiding another filter at every exec.
.section .pocket_probe,"ax",%progbits
.global pocket_filter_probe
.hidden pocket_filter_probe
.type pocket_filter_probe,%function
pocket_filter_probe:
    mov x8, #172
    svc #0
    ret
.size pocket_filter_probe, .-pocket_filter_probe
.section .note.GNU-stack,"",%progbits
