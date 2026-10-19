// SPDX-License-Identifier: GPL-3.0-only
// Guest-only wire header: carry PRoot's virtual UID/PID between guest services.
// These values are NOT authenticated identities and grant no host privileges.
// Host bootstrap messages use untouched, host-generated Parcel templates.
#if POCKET_GUEST_API != 28
#error Guest credential header must be reviewed for each framework ABI
#endif
#include <binder/IPCThreadState.h>
#include <binder/Parcel.h>
#include <unistd.h>

namespace android {
status_t Parcel::writeInterfaceToken(const String16& interface) {
    const uint64_t identity = (static_cast<uint64_t>(getuid()) << 32) | static_cast<uint32_t>(getpid());
    status_t result = writeInt64(static_cast<int64_t>(identity));
    if (result != NO_ERROR) return result;
    result = writeInt32(IPCThreadState::self()->getStrictModePolicy() | (0x40 << 16));
    return result == NO_ERROR ? writeString16(interface) : result;
}

bool Parcel::enforceInterface(const String16& interface, IPCThreadState* thread) const {
    if (dataAvail() < 16) return false;
    int64_t identity = readInt64();
    int32_t strictPolicy = readInt32();
    String16 received = readString16();
    if (errorCheck() != NO_ERROR || received != interface) return false;
    if (!thread) thread = IPCThreadState::self();
    // Binder's transaction dispatcher saves/restores caller state around
    // onTransact, including nested transactions. Reuse that mechanism.
    thread->restoreCallingIdentity(identity);
    thread->setStrictModePolicy((thread->getLastTransactionBinderFlags() & IBinder::FLAG_ONEWAY) ? 0 : strictPolicy);
    return true;
}
}
