// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <sys/syscall.h>
#include <unistd.h>
namespace pocket::fuse32 {
// Trusted UID0 runtime code may inspect carrier kernel FDs and prepare owned
// lower aliases during bootstrap. Ordinary guest
// UID0 commands still receive the directory VFS view. Scope is per-thread,
// nested, and cleared on fork; it does not change binder/guest UID context.
class ExternalKernelScope final {
    bool active_=false,okay_=true;
public:
    explicit ExternalKernelScope(bool requested=true) {
        if(!requested)return;
        if(syscall(__NR_getpid,0x504c1001UL)==0x504c1002UL) {
            active_=syscall(__NR_getpid,0x504c2001UL,8UL,1UL)==0;okay_=active_;
        }
    }
    ~ExternalKernelScope(){if(active_)(void)syscall(__NR_getpid,0x504c2001UL,8UL,0UL);}
    ExternalKernelScope(const ExternalKernelScope&)=delete;
    bool okay()const{return okay_;}
};
}
