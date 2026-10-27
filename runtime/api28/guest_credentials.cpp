// SPDX-License-Identifier: GPL-3.0-only
// Guest-only wire header: carry PRoot's virtual UID/PID between guest services.
// These values are NOT authenticated identities and grant no host privileges.
// Host bootstrap messages use untouched, host-generated Parcel templates.
#if POCKET_GUEST_API != 28 && POCKET_GUEST_API != 29
#error Guest credential header must be reviewed for each framework ABI
#endif
#include <binder/IPCThreadState.h>
#include <binder/Parcel.h>
#include <unistd.h>
#include "binder_token.h"

namespace android {
status_t Parcel::writeInterfaceToken(const String16& interface) {
    const uint64_t identity = (static_cast<uint64_t>(getuid()) << 32) | static_cast<uint32_t>(getpid());
    status_t result = writeInt64(static_cast<int64_t>(identity));
    if (result != NO_ERROR) return result;
    result = writeInt32(IPCThreadState::self()->getStrictModePolicy() | (0x40 << 16));
#if POCKET_GUEST_API == 29
    if (result != NO_ERROR) return result;
    updateWorkSourceRequestHeaderPosition();
    const IPCThreadState* thread = IPCThreadState::self();
    result = writeInt32(thread->shouldPropagateWorkSource() ?
        thread->getCallingWorkSourceUid() : IPCThreadState::kUnsetWorkSource);
#endif
    return result == NO_ERROR ? writeString16(interface) : result;
}

bool Parcel::enforceInterface(const String16& interface, IPCThreadState* thread) const {
    PocketBinderToken header;
    const size_t position=dataPosition();
    if (!pocket_binder_token(static_cast<const uint8_t*>(data())+position, dataAvail(), &header)) return false;
    setDataPosition(position+header.string_offset);
    String16 received = readString16();
    if (errorCheck() != NO_ERROR || received != interface) return false;
    if (!thread) thread = IPCThreadState::self();
    if (header.format==3 || header.format==4) thread->restoreCallingIdentity(static_cast<int64_t>(header.identity));
#if POCKET_GUEST_API == 29
    if (header.format==1 || header.format==2 || header.format==4) {
        // Keep Parcel's mutable header position valid for the platform's
        // readCallingWorkSourceUid/replaceCallingWorkSourceUid methods.
        const size_t end = dataPosition();
        setDataPosition(position+header.string_offset-4);
        updateWorkSourceRequestHeaderPosition();
        setDataPosition(end);
    }
    thread->setCallingWorkSourceUidWithoutPropagation(header.work_source);
#endif
    // Standard native parcels use the PID-derived guest identity established
    // by BBinder::transact, without requiring a patched client library.
    thread->setStrictModePolicy((thread->getLastTransactionBinderFlags() & IBinder::FLAG_ONEWAY) ? 0 : header.policy);
    return true;
}
}
