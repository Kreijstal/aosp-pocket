// SPDX-License-Identifier: GPL-3.0-only
// Rootless guest-role metadata, not authenticated host security principals.
// Kernel Binder reports PID zero for one-way calls, so PID-file lookup alone
// cannot preserve their logical caller. Host bootstrap uses raw host templates.
#if POCKET_GUEST_API != 32
#error Review both Parcel overloads and the RPC path for each platform ABI
#endif
#include "binder_token.h"
#include <binder/IPCThreadState.h>
#include <binder/Parcel.h>
#include <dlfcn.h>
#include <unistd.h>

namespace android {
status_t Parcel::writeInterfaceToken(const String16& interface) {
    return writeInterfaceToken(interface.string(),interface.size());
}
status_t Parcel::writeInterfaceToken(const char16_t* interface,size_t length) {
    using Original=status_t(*)(Parcel*,const char16_t*,size_t);
    static Original original=reinterpret_cast<Original>(dlsym(RTLD_NEXT,"_ZN7android6Parcel19writeInterfaceTokenEPKDsm"));
    if (!original) return INVALID_OPERATION;
    // Binder RPC has its own framing and identity model. Preserve its stock
    // implementation; only the kernel-Binder guest protocol gains this prefix.
    if (!isForRpc()) {
        uint64_t identity=(static_cast<uint64_t>(getuid())<<32)|static_cast<uint32_t>(getpid());
        status_t status=writeInt64(static_cast<int64_t>(identity));
        if (status!=NO_ERROR) return status;
    }
    // The real implementation writes strict policy, WorkSource and SYST, and
    // records the correct mutable WorkSource position after the added prefix.
    return original(this,interface,length);
}
bool Parcel::enforceInterface(const String16& interface,IPCThreadState* thread) const {
    return enforceInterface(interface.string(),interface.size(),thread);
}
bool Parcel::enforceInterface(const char16_t* interface,size_t length,IPCThreadState* thread) const {
    using Original=bool(*)(const Parcel*,const char16_t*,size_t,IPCThreadState*);
    static Original original=reinterpret_cast<Original>(dlsym(RTLD_NEXT,"_ZNK7android6Parcel16enforceInterfaceEPKDsmPNS_14IPCThreadStateE"));
    if (!original) return false;
    if (!isForRpc()) {
        PocketBinderToken header={};
        const size_t start=dataPosition();
        if (start<=dataSize() && dataAvail()>=24 &&
            pocket_binder_token(data()+start,dataAvail(),&header) && header.format==5)
            setDataPosition(start+sizeof(uint64_t));
    }
    // BBinder::transact validates the sender metadata and restores identity
    // around dispatch. Keep all stock API 32 descriptor/WorkSource validation.
    return original(this,interface,length,thread);
}
}
