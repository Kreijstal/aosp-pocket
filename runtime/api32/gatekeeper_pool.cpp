// SPDX-License-Identifier: GPL-3.0-only
// The unmodified SDK software HAL needs a worker while private registration
// verifies its remote interface chain. No credential operation is replaced.
#include <dlfcn.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
namespace android { namespace hardware {
void configureRpcThreadpool(size_t maximum,bool callerWillJoin) {
    using Configure=void(*)(size_t,bool);
    auto actual=reinterpret_cast<Configure>(dlsym(RTLD_NEXT,"_ZN7android8hardware22configureRpcThreadpoolEmb"));
    char sdk[PROP_VALUE_MAX];__system_property_get("ro.build.version.sdk",sdk);
    if(!actual || syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL || getuid()!=1000 || strcmp(sdk,"32"))_exit(125);
    if(maximum==1 && callerWillJoin) {
        maximum=2;
        fputs("API32_GATEKEEPER_RPC_POOL requested=1 actual=2 backend=stock_sdk_software\n",stderr);
    }
    actual(maximum,callerWillJoin);
}
}}
