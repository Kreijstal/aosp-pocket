// SPDX-License-Identifier: GPL-3.0-only
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include "ResourceManagerService.h"
#include <stdio.h>
int main() {
    android::ProcessState::self()->startThreadPool();
    if(android::ResourceManagerService::publish()!=android::OK)return 1;
    fputs("POCKET_MEDIA_RESOURCES_READY source=android-9.0.0_r61\n",stderr);
    android::IPCThreadState::self()->joinThreadPool();
    return 0;
}
