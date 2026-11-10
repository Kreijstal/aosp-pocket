// SPDX-License-Identifier: GPL-3.0-only
// Process-local guest authorization for stock 12L keystore2. No guest kernel
// policy is loaded; the outer Android app's SELinux policy remains enforced.
#include <binder/IPCThreadState.h>
#include <sys/system_properties.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>
#include <stdint.h>
#include "../api28/binder_identity.h"

namespace {
constexpr const char* appKeys="u:r:pocket_keystore:s0"; // A virtual label, not a kernel SID.
bool oneOf(const char* value,std::initializer_list<const char*> names) {
    for(const char* name:names)if(!strcmp(value,name))return true;
    return false;
}
bool caller(uid_t* uid) {
    auto* state=android::IPCThreadState::self();
    uint64_t identity=0;
    if(!pocket_binder_identity(state->getCallingPid(),&identity) ||
       (identity>>32)!=state->getCallingUid())return false;
    *uid=state->getCallingUid();return true;
}
__attribute__((constructor)) void requireKeystore32() {
    char sdk[PROP_VALUE_MAX];__system_property_get("ro.build.version.sdk",sdk);
    if(syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL || strcmp(sdk,"32") || getuid()!=1017) {
        fputs("API32_KEYSTORE_POLICY_REQUIRES_GUEST_KEYSTORE\n",stderr);_exit(125);
    }
    fputs("API32_KEYSTORE_POLICY guest_role_checks=1 kernel_policy=inherited hardware_security=not_claimed\n",stderr);
}
}

extern "C" int getcon(char** output) {
    if(!output){errno=EINVAL;return -1;}
    *output=strdup(appKeys);return *output?0:-1;
}
extern "C" const char* AIBinder_getCallingSid() {
    // The rootless Binder adapter restores logical UID/PID with
    // IPCThreadState::restoreCallingIdentity, which intentionally clears the
    // kernel SID. Keystore2 requires a context before dispatching its policy
    // check. Supply a virtual label only for a verified current guest caller;
    // authorization below still uses the UID, not this descriptive string.
    uid_t uid;
    return caller(&uid)?"u:r:pocket_guest:s0":nullptr;
}
extern "C" int selinux_check_access(const char* source,const char* target,const char* type,
                                    const char* permission,void*) {
    uid_t uid=static_cast<uid_t>(-1);bool allowed=false;
    if(source && target && type && permission && caller(&uid)) {
        const bool system=uid==0 || uid==1000;
        if(!strcmp(type,"keystore2") && !strcmp(target,appKeys)) {
            allowed=system && oneOf(permission,{"add_auth","clear_ns","get_state","list","lock","reset",
                "unlock","change_user","change_password","clear_uid","early_boot_ended","report_off_body"});
            // Pulling aggregate keystore metrics is also a system service operation.
            if(system && !strcmp(permission,"pull_metrics"))allowed=true;
        } else if(!strcmp(type,"keystore2_key")) {
            // Stock Rust checks APP namespace ownership and GRANT access vectors
            // independently. Isolated UIDs cannot create or use application keys.
            bool app=!strcmp(target,appKeys) && uid<INT32_MAX && uid%100000<99000;
            bool locksettings=uid==1000 && !strcmp(target,"u:object_r:locksettings_key:s0");
            bool wifi=uid==1010 && !strcmp(target,"u:object_r:wifi_key:s0");
            allowed=(app || locksettings || wifi) &&
                oneOf(permission,{"get_info","rebind","update","use","delete"});
            if(app && !strcmp(permission,"grant"))allowed=true;
            if(system && (app || locksettings) && !strcmp(permission,"req_forced_op"))allowed=true;
            // No raw FBE blobs, device-ID attestation, unique IDs or arbitrary
            // SELinux namespaces are authorized by this software profile.
        }
    }
    if(allowed)return 0;
    fprintf(stderr,"API32_KEYSTORE_POLICY_DENIED uid=%u class=%s permission=%s\n",uid,
        type?type:"null",permission?permission:"null");
    errno=EACCES;return -1;
}
