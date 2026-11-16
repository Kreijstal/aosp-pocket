// SPDX-License-Identifier: GPL-3.0-only
// Exercise the real policy implementation without a daemon or any stored keys.
#include <binder/IPCThreadState.h>
#include "../direct/binder_dispatch.h"
#include <assert.h>
#include <initializer_list>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
static uint32_t actualUid=1017,actualPid=1234;
android::IPCThreadState* android::IPCThreadState::self(){static IPCThreadState state;return &state;}
uid_t android::IPCThreadState::getCallingUid()const{return actualUid;}
pid_t android::IPCThreadState::getCallingPid()const{return actualPid;}
extern "C" uid_t __wrap_getuid(){return 1017;}
extern "C" long __wrap_syscall(long number,...){assert(number==SYS_getpid);return 0x504c1002UL;}
extern "C" int __system_property_get(const char* name,char* value){assert(!strcmp(name,"ro.build.version.sdk"));strcpy(value,"32");return 2;}
#ifndef TEST_NO_ADAPTER
extern "C" int pocket_binder_guest_caller(uint32_t* uid,uint32_t* pid) {
    return PocketBinderDispatchScope::caller(actualUid,actualPid,uid,pid)?1:0;
}
#endif
extern "C" const char* AIBinder_getCallingSid();
extern "C" int selinux_check_access(const char*,const char*,const char*,const char*,void*);
static const char* keys="u:r:pocket_keystore:s0";
static unsigned checks;
static void permission(bool allowed,const char* type,const char* operation,const char* target=keys) {
    errno=0;int result=selinux_check_access("u:r:pocket_guest:s0",target,type,operation,nullptr);
    assert(result==(allowed?0:-1));if(!allowed)assert(errno==EACCES);checks++;
}
static void sid(bool present){assert((AIBinder_getCallingSid()!=nullptr)==present);checks++;}
static uint64_t identity(uint32_t uid){return (uint64_t(uid)<<32)|actualPid;}
int main() {
    sid(false);permission(false,"keystore2","reset");
#ifdef TEST_NO_ADAPTER
    actualUid=1000;PocketBinderDispatchScope context(true,identity(actualUid));
    sid(false);permission(false,"keystore2","get_state");permission(false,"keystore2_key","use");
    printf("PASS: Keystore missing-adapter policy %u cases\n",checks);
#else
    {
        actualUid=1000;PocketBinderDispatchScope context(true,identity(actualUid));sid(true);
        permission(true,"keystore2","get_state");permission(true,"keystore2","reset");
        permission(true,"keystore2","pull_metrics");permission(false,"keystore2","unknown");
        permission(true,"keystore2_key","rebind","u:object_r:locksettings_key:s0");
        permission(false,"keystore2_key","use","u:object_r:wifi_key:s0");
        {PocketBinderDispatchScope host(false,identity(actualUid));sid(false);permission(false,"keystore2","reset");}
        sid(true);permission(true,"keystore2","get_state");
    }
    {
        actualUid=10114;PocketBinderDispatchScope context(true,identity(actualUid));sid(true);
        for(const char* operation:{"get_info","rebind","update","use","delete","grant"})permission(true,"keystore2_key",operation);
        for(const char* operation:{"get_state","reset","clear_uid","add_auth","pull_metrics"})permission(false,"keystore2",operation);
        permission(false,"keystore2_key","gen_unique_id");permission(false,"keystore2_key","req_forced_op");
        permission(false,"keystore2_key","use","u:object_r:locksettings_key:s0");
        permission(false,"keystore2_key","use","u:object_r:wifi_key:s0");
        actualUid=0;sid(false);permission(false,"keystore2","reset");actualUid=10114;sid(true);
    }
    for(uint32_t isolated:{99000u,199999u}) {
        actualUid=isolated;PocketBinderDispatchScope context(true,identity(actualUid));sid(true);
        permission(false,"keystore2_key","rebind");permission(false,"keystore2_key","use");
        permission(false,"keystore2","get_state");
    }
    {
        actualUid=1010;PocketBinderDispatchScope context(true,identity(actualUid));sid(true);
        permission(true,"keystore2_key","use","u:object_r:wifi_key:s0");
        permission(false,"keystore2_key","use","u:object_r:locksettings_key:s0");
    }
    sid(false);permission(false,"keystore2_key","use");
    printf("PASS: Keystore caller/role policy %u cases\n",checks);
#endif
}
