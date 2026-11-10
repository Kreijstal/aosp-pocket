// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "service_activation.h"
#include <sys/system_properties.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
namespace pocket {
inline bool lazyMediaEnabled32() {
    char value[PROP_VALUE_MAX];__system_property_get("ro.pocket.lazy_media",value);
    return !strcmp(value,"1") || !strcmp(value,"true");
}
inline void requestLazyService32(const char* name,bool hardware=false) {
    const char* shared=getenv("POCKET_API32_METRICS_IN_CORE");
    const auto* service=lazyService32(name,hardware,shared && !strcmp(shared,"1"));
    if(!service || !lazyMediaEnabled32())return;
    char value[PROP_VALUE_MAX];__system_property_get("ro.pocket.media.enabled",value);
    const bool media=!strcmp(value,"1") || !strcmp(value,"true");
    timespec now{};if(clock_gettime(CLOCK_MONOTONIC,&now))return;
    static LazyServiceStarter32 starter;
    starter.request(service,true,media,uint64_t(now.tv_sec)*1000+now.tv_nsec/1000000,[](const char* target){
        int result=__system_property_set("ctl.start",target);
        fprintf(stderr,"API32_LAZY_SERVICE_REQUEST service=%s result=%d\n",target,result);
    });
}
}
