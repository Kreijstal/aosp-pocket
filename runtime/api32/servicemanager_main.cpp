// SPDX-License-Identifier: GPL-3.0-only
#include "service_registry.h"
#include "../api28/binder_bridge.h"
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <sys/system_properties.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

using namespace android;
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);char sdk[PROP_VALUE_MAX];
    __system_property_get("ro.build.version.sdk",sdk);
    if(strcmp(sdk,"32") || getuid()!=0)return 64;
    ProcessState::self()->startThreadPool();
    if(argc==3 && !strcmp(argv[1],"wait")) {
        auto manager=defaultServiceManager();if(!manager)return 2;
        for(unsigned i=0;i<200;++i) {
            auto service=manager->checkService(String16(argv[2]));
            if(service && service->pingBinder()==NO_ERROR) {
                printf("POCKET_RUNTIME_SERVICE_READY name=%s\n",argv[2]);return 0;
            }
            usleep(50000);
        }
        fprintf(stderr,"POCKET_RUNTIME_SERVICE_TIMEOUT name=%s\n",argv[2]);return 3;
    }
    if(argc!=1)return 64;
    status_t status=pocket::publishManager(pocket::createServiceRegistry32());
    if(status!=NO_ERROR){fprintf(stderr,"POCKET_SERVICEMANAGER_FAILED status=%d\n",status);return 1;}
    puts("POCKET_SERVICEMANAGER_READY sdk=32 private_registry=1");
    IPCThreadState::self()->joinThreadPool();return 0;
}
