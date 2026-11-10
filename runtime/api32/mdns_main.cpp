#include "service_output.h"
// SPDX-License-Identifier: GPL-3.0-only
// Keep the stock 12L text command frontend and DNS-SD client implementation.
#include "MDnsSdListener.h"
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main() {
    pocketServiceOutput32();char sdk[PROP_VALUE_MAX];
    if(getuid()!=0 || syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL ||
        __system_property_get("ro.build.version.sdk",sdk)<=0 || strcmp(sdk,"32"))return 64;
    auto* listener=new MDnsSdListener;
    if(listener->startListener()){perror("API32_MDNS_LISTENER_START");return 1;}
    puts("API32_MDNS_CONTROL_READY stock_listener=12L");
    for(;;)pause();
}
