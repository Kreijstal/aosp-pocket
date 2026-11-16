// Test-only IPC state; production builds use the pinned platform header.
#pragma once
#include <sys/types.h>
namespace android {
class IPCThreadState {
public:
    static IPCThreadState* self();
    uid_t getCallingUid() const;
    pid_t getCallingPid() const;
};
}
