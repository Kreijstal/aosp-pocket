// SPDX-License-Identifier: GPL-3.0-only
// Matching 12L default/service.cpp flow with bounded startup diagnostics.
// The factory and all key operations come from the immutable SDK libraries.
#include <AndroidKeymaster41Device.h>
#include <hidl/HidlTransportSupport.h>
#include <sys/system_properties.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

int main() {
    setbuf(stdout,nullptr);char value[PROP_VALUE_MAX];
    __system_property_get("ro.build.version.sdk",value);
    if(syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL || getuid()!=1000 || strcmp(value,"32"))return 64;
    puts("API32_KEYMASTER_START backend=stock_software");
    for(const char* property:{"ro.build.version.release","ro.build.version.security_patch"}) {
        const prop_info* entry=__system_property_find(property);
        printf("API32_KEYMASTER_IMAGE_PROP name=%s present=%d\n",property,entry!=nullptr);
        if(!entry)return 65;
    }
    // Pocket bootstraps the private manager through an asynchronous Binder
    // reply before registerAsService returns. With stock (1,true), libhwbinder
    // starts no worker until main joins, so that reply cannot be received.
    android::hardware::configureRpcThreadpool(2,true);
    puts("API32_KEYMASTER_RPC_CONFIGURED");
    android::sp<android::hardware::keymaster::V4_1::IKeymasterDevice> device=
        keymaster::V4_1::CreateKeymasterDevice(android::hardware::keymaster::V4_0::SecurityLevel::SOFTWARE);
    if(!device)return 66;
    puts("API32_KEYMASTER_CREATED");
    const auto status=device->registerAsService();
    printf("API32_KEYMASTER_REGISTER_RESULT status=%d\n",status);
    if(status!=android::OK)return 67;
    android::hardware::joinRpcThreadpool();return 68;
}
