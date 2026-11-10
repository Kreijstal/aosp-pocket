// SPDX-License-Identifier: GPL-3.0-only
#include <vintf/RuntimeInfo.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>

using namespace android;
using namespace android::vintf;
struct RuntimeProbe : RuntimeInfo {
    using RuntimeInfo::fetchAllInformation;
    using RuntimeInfo::parseGkiKernelRelease;
};
#define CHECK(condition) do {if(!(condition)){fprintf(stderr,"API32_VINTF_PROBE_FAILURE line=%d\n",__LINE__);return 1;}}while(0)
int main() {
    CHECK(syscall(__NR_getpid,0x504c1001UL)==0x504c1002UL);
    CHECK(RuntimeInfo::gkiAndroidReleaseToLevel(11)==Level::UNSPECIFIED);
    CHECK(RuntimeInfo::gkiAndroidReleaseToLevel(12)==Level::S);
    CHECK(RuntimeInfo::gkiAndroidReleaseToLevel(13)==Level::T);
    CHECK(RuntimeInfo::gkiAndroidReleaseToLevel(16)==Level::UNSPECIFIED);
    CHECK(RuntimeInfo::gkiAndroidReleaseToLevel(UINT64_MAX)==Level::UNSPECIFIED);
    KernelVersion version;Level level=Level::UNSPECIFIED;
    constexpr auto flags=RuntimeInfo::CPU_VERSION|RuntimeInfo::KERNEL_FCM;
    CHECK(RuntimeProbe::parseGkiKernelRelease(flags,"5.10.107-android12-0",&version,&level)==OK);
    CHECK(version==KernelVersion(5,10,107) && level==Level::S);
    level=Level::R;
    CHECK(RuntimeProbe::parseGkiKernelRelease(flags,"6.12.38-android16-0",&version,&level)!=OK);
    CHECK(version==KernelVersion(6,12,38) && level==Level::R);
    level=Level::UNSPECIFIED;
    CHECK(RuntimeProbe::parseGkiKernelRelease(RuntimeInfo::CPU_VERSION,"6.12.38-android16-0",&version,&level)==OK);
    CHECK(version==KernelVersion(6,12,38) && level==Level::UNSPECIFIED);
    utsname actual{};CHECK(!uname(&actual));
    RuntimeProbe runtime;CHECK(runtime.fetchAllInformation(RuntimeInfo::CPU_VERSION)==OK);
    CHECK(runtime.osRelease()==actual.release);
    unsigned long long major,minor,patch;
    CHECK(sscanf(actual.release,"%llu.%llu.%llu",&major,&minor,&patch)==3);
    CHECK(runtime.kernelVersion()==KernelVersion(major,minor,patch));
    puts("API32_VINTF_KERNEL_PASS stock_parser=1 known_levels=preserved future_level=error overflow=rejected uname=actual kernel_version=actual compatibility_not_forged=1");
    return 0;
}
