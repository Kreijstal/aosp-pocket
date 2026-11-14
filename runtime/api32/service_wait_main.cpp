// SPDX-License-Identifier: GPL-3.0-only
#include "service_readiness.h"
#include <binder/IBinder.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <sys/system_properties.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

using namespace android;
static uint64_t monotonicMs() {
    timespec time{};
    if(clock_gettime(CLOCK_MONOTONIC,&time)!=0)_exit(70);
    return uint64_t(time.tv_sec)*1000+uint64_t(time.tv_nsec)/1000000;
}
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);
    char sdk[PROP_VALUE_MAX];__system_property_get("ro.build.version.sdk",sdk);
    if(argc!=2 || !argv[1][0] || strlen(argv[1])>256 || strcmp(sdk,"32") || getuid()!=0)return 64;
    if(!strcmp(argv[1],"media.extractor")) {
        char hardware[PROP_VALUE_MAX]{},codec[PROP_VALUE_MAX]{};
        __system_property_get("ro.hw_timeout_multiplier",hardware);
        __system_property_get("debug.stagefright.ccodec_timeout_mult",codec);
        if(hardware[0]<'1' || hardware[0]>'4' || hardware[1] || strcmp(hardware,codec)) {
            fprintf(stderr,"POCKET_CODEC_TIMEOUT_POLICY_FAILED hardware_multiplier=%s codec_multiplier=%s\n",hardware,codec);
            return 65;
        }
        printf("POCKET_CODEC_TIMEOUT_POLICY hardware_multiplier=%s codec_multiplier=%s nominal_operation_ms=%u watchdog_enabled=1\n",
               hardware,codec,1500u*static_cast<unsigned>(codec[0]-'0'));
    }
    ProcessState::self()->startThreadPool();
    auto manager=defaultServiceManager();if(!manager)return 2;
    constexpr uint64_t timeoutMs=60000,intervalMs=50;
    unsigned published=0;status_t lastStatus=NAME_NOT_FOUND;
    printf("POCKET_RUNTIME_SERVICE_WAIT name=%s timeout_ms=%" PRIu64 " monotonic_ms=%" PRIu64 "\n",argv[1],timeoutMs,monotonicMs());
    auto result=pocket::awaitServiceReadiness(timeoutMs,intervalMs,monotonicMs,[&] {
        auto service=manager->checkService(String16(argv[1]));
        if(!service){lastStatus=NAME_NOT_FOUND;return false;}
        ++published;lastStatus=service->pingBinder();return lastStatus==NO_ERROR;
    },[](uint64_t ms){usleep(static_cast<useconds_t>(ms*1000));});
    printf("POCKET_RUNTIME_SERVICE_WAIT_RESULT name=%s result=%s elapsed_ms=%" PRIu64 " checks=%u published=%u last_status=%d\n",
           argv[1],result.ready?"ready":"timeout",result.elapsedMs,result.checks,published,lastStatus);
    if(result.ready){printf("POCKET_RUNTIME_SERVICE_READY name=%s\n",argv[1]);return 0;}
    fprintf(stderr,"POCKET_RUNTIME_SERVICE_TIMEOUT name=%s\n",argv[1]);return 3;
}
