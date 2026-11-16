// SPDX-License-Identifier: GPL-3.0-only
#include "../direct/binder_dispatch.h"
#include <assert.h>
#include <thread>
#include <stdio.h>
static unsigned checks;
static uint64_t identity(uint32_t uid,uint32_t pid){return (uint64_t(uid)<<32)|pid;}
static void expect(bool allowed,uint32_t actualUid=10108,uint32_t actualPid=1234) {
    uint32_t uid=UINT32_MAX,pid=UINT32_MAX;
    assert(PocketBinderDispatchScope::caller(actualUid,actualPid,&uid,&pid)==allowed);
    if(allowed)assert(uid==actualUid && pid==actualPid);
    else assert(uid==UINT32_MAX && pid==UINT32_MAX);
    checks++;
}
int main() {
    expect(false);
    {
        PocketBinderDispatchScope outer(true,identity(10108,1234));expect(true);
        expect(false,1000,1234);expect(false,10108,5678); // Cleared or different identity.
        uint32_t value;assert(!PocketBinderDispatchScope::caller(10108,1234,nullptr,&value));checks++;
        assert(!PocketBinderDispatchScope::caller(10108,1234,&value,nullptr));checks++;
        {PocketBinderDispatchScope host(false,identity(10108,1234));expect(false);}
        expect(true);
        {PocketBinderDispatchScope nested(true,identity(1000,5678));expect(true,1000,5678);expect(false);}
        expect(true);
        {PocketBinderDispatchScope invalid(true,identity(UINT32_MAX,1234));expect(false,UINT32_MAX,1234);}
        {PocketBinderDispatchScope invalid(true,identity(10108,0));expect(false,10108,0);}
        bool threadInitiallyEmpty=false,threadHasOwnContext=false;
        std::thread other([&]{
            uint32_t uid,pid;
            threadInitiallyEmpty=!PocketBinderDispatchScope::caller(10108,1234,&uid,&pid);
            PocketBinderDispatchScope nested(true,identity(39999,9876));
            threadHasOwnContext=PocketBinderDispatchScope::caller(39999,9876,&uid,&pid) && uid==39999 && pid==9876;
        });other.join();assert(threadInitiallyEmpty && threadHasOwnContext);checks+=2;
        expect(true);
    }
    expect(false);
    {PocketBinderDispatchScope system(true,identity(0,1));expect(true,0,1);}
    {PocketBinderDispatchScope upper(true,identity(INT32_MAX,1234));expect(true,INT32_MAX,1234);}
    expect(false);
    printf("PASS: Binder dispatch context %u cases (nesting, host rejection, identity changes, thread isolation)\n",checks);
}
