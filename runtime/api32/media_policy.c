// SPDX-License-Identifier: GPL-3.0-only
// Rootless 12L media services: stock mediaswcodec and mediaextractor call
// android::SetUpMinijail() to install a seccomp filter with a SIGSYS logging
// handler. Under Pocket, the loader already owns SIGSYS and the guest cannot
// install kernel seccomp filters for logical guest roles, so minijail aborts
// the service. This preload replaces only those two entry points. The outer
// app UID remains the host security boundary; no guest isolation is claimed.
#include <stdio.h>

#define EXPORT __attribute__((visibility("default")))

// android::SetUpMinijail(const std::string&, const std::string&)
EXPORT void _ZN7android13SetUpMinijailERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEES8_(
        const void* basePolicy, const void* additionalPolicy) {
    (void)basePolicy; (void)additionalPolicy;
    fputs("POCKET_MEDIA_SECCOMP_UNAVAILABLE service continues without a guest seccomp filter\n", stderr);
}

// android::SetUpMinijailList(const std::string&, const std::vector<std::string>&)
EXPORT void _ZN7android17SetUpMinijailListERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEERKNS0_6vectorIS6_NS4_IS6_EEEE(
        const void* basePolicy, const void* additionalPolicies) {
    (void)basePolicy; (void)additionalPolicies;
    fputs("POCKET_MEDIA_SECCOMP_UNAVAILABLE service continues without a guest seccomp filter\n", stderr);
}
