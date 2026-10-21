// SPDX-License-Identifier: GPL-3.0-only
// Decode guest credentials before services perform permission checks. All guest
// roles share one real host app UID; these are not host security principals.
#include <binder/Binder.h>
#include <binder/IPCThreadState.h>
#include <binder/Parcel.h>
#include <dlfcn.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "kernel_identity.h"
namespace android {
status_t BBinder::transact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags) {
    using Original=status_t(*)(BBinder*,uint32_t,const Parcel&,Parcel*,uint32_t);
    static Original original=reinterpret_cast<Original>(dlsym(RTLD_NEXT,"_ZN7android7BBinder8transactEjRKNS_6ParcelEPS1_j"));
    if (!original) return INVALID_OPERATION;
    IPCThreadState* thread=IPCThreadState::self();
    bool guest=false;int64_t identity=0;
    static const uid_t kernelUid=pocket_kernel_uid_from_proc();
    if (kernelUid!=static_cast<uid_t>(-1) && code>=FIRST_CALL_TRANSACTION && code<=LAST_CALL_TRANSACTION && data.dataSize()>=16 &&
        thread->getCallingUid()==kernelUid) {
        uint64_t claimed;uint32_t policy;int32_t tokenLength;
        memcpy(&claimed,data.data(),8);memcpy(&policy,data.data()+8,4);memcpy(&tokenLength,data.data()+12,4);
        // The kernel reports PID zero for one-way calls. Their claimed PID is
        // guest metadata; only synchronous sender PIDs can be kernel-checked.
        // Host broadcasts and native Binder control messages remain excluded.
        bool senderPidMatches=static_cast<uint32_t>(claimed)==static_cast<uint32_t>(thread->getCallingPid()) ||
            ((flags&IBinder::FLAG_ONEWAY) && thread->getCallingPid()==0 && static_cast<uint32_t>(claimed)>0);
        guest=(policy&0x00400000u)!=0 && tokenLength>0 &&
            static_cast<uint32_t>(tokenLength)<=(data.dataSize()-16)/2 &&
            senderPidMatches &&
            (claimed>>32)<=0x7fffffffu;
        if (guest) {
            identity=thread->clearCallingIdentity();
            thread->restoreCallingIdentity(static_cast<int64_t>(claimed));
        }
    }
    status_t result=original(this,code,data,reply,flags);
    if (guest) thread->restoreCallingIdentity(identity);
    return result;
}
}
