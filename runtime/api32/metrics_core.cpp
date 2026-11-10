// SPDX-License-Identifier: GPL-3.0-only
// Host the stock 12L implementation in the already supervised native core.
#include <MediaMetricsService.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <stdio.h>
#include <unistd.h>

int pocketStartMediaMetricsInCore32() {
    using namespace android;
    auto manager=defaultServiceManager();
    if(!manager)return 1;
    sp<IBinder> service=new MediaMetricsService();
    status_t status=manager->addService(String16(MediaMetricsService::kServiceName),service);
    if(status!=NO_ERROR)return 2;
    // The standalone main's argv rename and process-wide 128-MiB address
    // limit cannot apply to a core shared with other services. The stock
    // service's record limits, Binder dispatch and caller checks are retained.
    printf("POCKET_CORE_METRICS_READY pid=%d logical_uid=%u caller_checks=stock\n",getpid(),getuid());
    IPCThreadState::self()->joinThreadPool();return 3;
}
