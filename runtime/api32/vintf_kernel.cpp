// SPDX-License-Identifier: GPL-3.0-only
// Forward-compatible failure for a 12L framework on a newer host GKI kernel.
#include <vintf/RuntimeInfo.h>
#include <inttypes.h>
#include <stdio.h>

namespace android::vintf {
Level RuntimeInfo::gkiAndroidReleaseToLevel(uint64_t androidRelease) {
    constexpr uint64_t firstRelease=12;
    constexpr uint64_t firstLevel=static_cast<uint64_t>(Level::S);
    constexpr uint64_t count=static_cast<uint64_t>(Level::LAST_PLUS_ONE)-firstLevel;
    if(androidRelease<firstRelease)return Level::UNSPECIFIED;
    if(androidRelease-firstRelease>=count) {
        // parseGkiKernelRelease already handles UNSPECIFIED as an error.
        // Do not fabricate an older FCM level or alter uname/kernel identity.
        fprintf(stderr,"API32_VINTF_KERNEL_LEVEL_UNKNOWN android_release=%" PRIu64 " fcm=unspecified\n",androidRelease);
        return Level::UNSPECIFIED;
    }
    return static_cast<Level>(androidRelease-firstRelease+firstLevel);
}
}
