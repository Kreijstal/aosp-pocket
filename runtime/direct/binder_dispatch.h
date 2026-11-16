// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

// Guest-role metadata scoped to dispatch; no host identity or new isolation boundary.
class PocketBinderDispatchScope {
    const PocketBinderDispatchScope* previous;
    const bool guest;
    const uint64_t identity;
    static const PocketBinderDispatchScope*& current() {
        static thread_local const PocketBinderDispatchScope* value=nullptr;
        return value;
    }
public:
    PocketBinderDispatchScope(bool accepted,uint64_t caller)
        :previous(current()),guest(accepted),identity(caller) {current()=this;}
    ~PocketBinderDispatchScope(){current()=previous;}
    PocketBinderDispatchScope(const PocketBinderDispatchScope&)=delete;
    PocketBinderDispatchScope& operator=(const PocketBinderDispatchScope&)=delete;
    static bool caller(uint32_t actualUid,uint32_t actualPid,uint32_t* uid,uint32_t* pid) {
        const auto* frame=current();
        if(!uid || !pid || !frame || !frame->guest)return false;
        const uint32_t logicalUid=frame->identity>>32,logicalPid=frame->identity;
        // clearCallingIdentity must also suspend the original caller context.
        if(logicalUid>INT32_MAX || !logicalPid || logicalUid!=actualUid || logicalPid!=actualPid)return false;
        *uid=logicalUid;*pid=logicalPid;return true;
    }
};

// Provided by libpocket_binder_api32; absent adapters must fail closed.
using PocketBinderGuestCaller=int(*)(uint32_t*,uint32_t*);
