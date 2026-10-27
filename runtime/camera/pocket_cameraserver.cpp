// SPDX-License-Identifier: GPL-3.0-only
// 64-bit cameraserver for the ARM64-only guest. Same CameraService as AOSP main_cameraserver.cpp;
// its "legacy/0" provider is the in-process Pocket host camera provider.
// First, before any HIDL header declares android::hardware::Parcel: this header names Parcel unqualified
// inside android::hardware and means android::Parcel.
#include <camera/VendorTagDescriptor.h>
#include "hidl_codegen_compat.h"
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <hidl/HidlTransportSupport.h>
#include <signal.h>
#include <stdio.h>
#include "CameraService.h"
using namespace android;
int main() {
    signal(SIGPIPE, SIG_IGN);
    // Set 3 threads for HIDL calls
    hardware::configureRpcThreadpool(3, /*willjoin*/ false);
    sp<ProcessState> proc(ProcessState::self());
    CameraService::instantiate();
    fprintf(stderr, "POCKET_CAMERASERVER_READY\n");
    ProcessState::self()->startThreadPool();
    IPCThreadState::self()->joinThreadPool();
}
