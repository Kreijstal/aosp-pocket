// SPDX-License-Identifier: GPL-3.0-only
// 64-bit audioserver for the ARM64-only guest. Same AudioFlinger and
// AudioPolicyService as AOSP main_audioserver.cpp. Not started here:
// media.log (needs ro.test_harness), AAudio (MMAP policy is NEVER) and sound
// trigger (no HAL).
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <binder/IServiceManager.h>
#include <hidl/HidlTransportSupport.h>
#include <signal.h>
#include <stdio.h>
#include "AudioFlinger.h"
#include "AudioPolicyService.h"
using namespace android;
int main() {
    signal(SIGPIPE,SIG_IGN);
    android::hardware::configureRpcThreadpool(4,false);
    sp<ProcessState> proc(ProcessState::self());
    AudioFlinger::instantiate();
    AudioPolicyService::instantiate();
    fprintf(stderr,"POCKET_AUDIOSERVER_READY\n");
    ProcessState::self()->startThreadPool();
    IPCThreadState::self()->joinThreadPool();
}
