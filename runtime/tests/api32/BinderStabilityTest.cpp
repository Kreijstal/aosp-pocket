// SPDX-License-Identifier: GPL-3.0-only
#include "../../api32/binder_stability.h"
#include <stdio.h>

constexpr bool converts(int api,int32_t wire,bool null,int32_t expected) {
    int32_t actual=0;
    return pocket_api32_host_stability(api,wire,null,&actual) && actual==expected;
}
constexpr bool rejects(int api,int32_t wire,bool null) {
    int32_t actual=0;
    return !pocket_api32_host_stability(api,wire,null,&actual);
}
// Fixed AOSP wire examples: legacy has no trailer, 12L uses 0xLL000001,
// Android 11/13+ use the plain level LL. Null 12L binders still need version 1.
static_assert(converts(28,0,false,0x0c000001));
static_assert(converts(30,12,false,0x0c000001));
static_assert(converts(31,0x0c000001,false,0x0c000001));
static_assert(converts(32,0x3f000001,false,0x3f000001));
static_assert(converts(36,12,false,0x0c000001));
static_assert(converts(36,3,false,0x03000001));
static_assert(converts(36,0,false,0x0c000001));
static_assert(converts(29,0,true,1));
static_assert(converts(32,1,true,1));
static_assert(converts(36,0,true,1));
static_assert(rejects(27,0,false));
static_assert(rejects(32,0,false));
static_assert(rejects(32,0x0c000001,true));
static_assert(rejects(32,0x0c000101,false));
static_assert(rejects(36,42,false));
static_assert(rejects(36,12,true));
int main() { puts("API32_HOST_BINDER_STABILITY_TEST_PASS"); }
