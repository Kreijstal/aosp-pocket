// SPDX-License-Identifier: GPL-3.0-only
// One UID-1000 process for the software HALs and optional classic frontend.
#include <AndroidKeymaster41Device.h>
#include <SoftGateKeeperDevice.h>
#include <hidl/HidlTransportSupport.h>
#include <sys/system_properties.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#ifdef POCKET_CRYPTO_FRONTEND
#include <atomic>
#include <thread>
#include <time.h>
int pocket_gatekeeper_frontend_main(void (*ready)());
static std::atomic<bool> frontendReady{false};
static void frontend_registered() {
    printf("POCKET_GATEKEEPER_FRONTEND_READY pid=%d uid=%u gid=%u state=/data/misc/gatekeeper\n",
        getpid(),getuid(),getgid());
    frontendReady.store(true,std::memory_order_release);
}
static bool start_frontend() {
    std::thread([] {
        int status=pocket_gatekeeper_frontend_main(frontend_registered);
        fprintf(stderr,"POCKET_GATEKEEPER_FRONTEND_EXIT status=%d\n",status);
        // A missing credential service must terminate the supervised host.
        _exit(70);
    }).detach();
    struct timespec start{},now{},pause{0,10000000};
    if(clock_gettime(CLOCK_MONOTONIC,&start))return false;
    while(!frontendReady.load(std::memory_order_acquire)) {
        if(clock_gettime(CLOCK_MONOTONIC,&now) || now.tv_sec-start.tv_sec>=30)return false;
        nanosleep(&pause,nullptr);
    }
    return true;
}
#endif

int main() {
    setbuf(stdout,nullptr);char sdk[PROP_VALUE_MAX];
    __system_property_get("ro.build.version.sdk",sdk);
    if(syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL ||
            getuid()!=1000 || getgid()!=1000 || strcmp(sdk,"32"))return 64;
    unsigned components=2;
#ifdef POCKET_CRYPTO_FRONTEND
    components=3;
#endif
    printf("POCKET_CRYPTO_START pid=%d uid=%u gid=%u components=%u\n",getpid(),getuid(),getgid(),components);
    for(const char* property:{"ro.build.version.release","ro.build.version.security_patch"})
        if(!__system_property_find(property))return 65;
    // As in the standalone HALs, registration needs a worker before main joins.
    android::hardware::configureRpcThreadpool(2,true);
    android::sp<android::hardware::keymaster::V4_1::IKeymasterDevice> keymasterDevice=
        keymaster::V4_1::CreateKeymasterDevice(
        android::hardware::keymaster::V4_0::SecurityLevel::SOFTWARE);
    if(!keymasterDevice)return 66;
    auto status=keymasterDevice->registerAsService();
    printf("API32_KEYMASTER_REGISTER_RESULT status=%d\n",status);
    if(status!=android::OK)return 67;
    android::sp<android::SoftGateKeeperDevice> gatekeeper=new android::SoftGateKeeperDevice;
    status=gatekeeper->registerAsService();
    printf("API32_GATEKEEPER_REGISTER_RESULT status=%d\n",status);
    if(status!=android::OK)return 68;
#ifdef POCKET_CRYPTO_FRONTEND
    if(!start_frontend())return 71;
#endif
    puts("POCKET_CRYPTO_READY keymaster=stock_sdk_software gatekeeper=pinned_aosp_software scrypt=pinned_aosp_arm64");
    android::hardware::joinRpcThreadpool();return 69;
}
