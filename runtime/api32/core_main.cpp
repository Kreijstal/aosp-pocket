// SPDX-License-Identifier: GPL-3.0-only
// UID-0 services share one process; their existing typed dispatch stays intact.
#include "service_registry.h"
#include "../api28/binder_bridge.h"
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <sys/system_properties.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <unistd.h>

int pocket_core_netd();
int pocket_core_dns();
int pocket_core_storage();
int pocket_core_mdns();
int pocket_core_memory(int,char**);
int pocket_core_apex(int,char**);
extern "C" int pocketStartHardwareServicesInCore32();
int pocketStartMediaMetricsInCore32();
#ifdef POCKET_CORE_INSTALLD
extern "C" int pocketPrepareInstalldInCore32();
extern "C" int pocketStartInstalldInCore32();
#endif
#ifdef POCKET_CORE_TEST_ECHO
android::sp<android::IBinder> pocketCreateCoreTestEcho32();
#endif
using namespace android;
namespace {
int memory() {char name[]="pocket-memory-api32";char* args[]={name,nullptr};return pocket_core_memory(1,args);}
int apex() {char name[]="pocket-apex-service-api32",serve[]="serve";char* args[]={name,serve,nullptr};return pocket_core_apex(2,args);}
struct Component {const char* name;int (*run)();};
const Component components[]={{"pocket-netd",pocket_core_netd},{"pocket-dns",pocket_core_dns},
    {"pocket-storage",pocket_core_storage},{"pocket-mdns",pocket_core_mdns},
    {"pocket-memory",memory},{"pocket-apex",apex},
    {"pocket-hardware",pocketStartHardwareServicesInCore32}};
}
int main(int argc,char**) {
    setbuf(stdout,nullptr);char sdk[PROP_VALUE_MAX];__system_property_get("ro.build.version.sdk",sdk);
    if(argc!=1 || strcmp(sdk,"32") || getuid()!=0 ||
            syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL)return 64;
    const char* installd=getenv("POCKET_API32_INSTALLD_IN_CORE");
    if(installd && strcmp(installd,"1"))return 64;
    const bool sharedInstalld=installd!=nullptr;
#ifdef POCKET_CORE_INSTALLD
    if(sharedInstalld) {
        if(getgid()!=0 || pocketPrepareInstalldInCore32()!=0)return 65;
        // This is process-wide in libbinder. Choose the shared system-daemon
        // minimum priority before any Binder pool or component thread starts.
        // Like standalone installd, new local endpoints use nice 0, not 19.
        IPCThreadState::self()->disableBackgroundScheduling(true);
    }
#else
    if(sharedInstalld)return 64;
#endif
    ProcessState::self()->startThreadPool();
    sp<IBinder> registry=pocket::createServiceRegistry32();
    status_t result=pocket::publishManager(registry);if(result!=NO_ERROR)return 1;
    puts("POCKET_SERVICEMANAGER_READY sdk=32 private_registry=1");
#ifdef POCKET_CORE_TEST_ECHO
    auto manager=defaultServiceManager();
    if(!manager || manager->addService(String16("pocket.api32.echo"),pocketCreateCoreTestEcho32())!=NO_ERROR)return 2;
    puts("API32_READY");
    constexpr int testEcho=1;
#else
    constexpr int testEcho=0;
#endif
    const char* metrics=getenv("POCKET_API32_METRICS_IN_CORE");
    const bool sharedMetrics=metrics && !strcmp(metrics,"1");
    printf("POCKET_CORE_START pid=%d uid=%u components=%d test_echo=%d\n",
           getpid(),getuid(),8+int(sharedMetrics)+int(sharedInstalld),testEcho);
    const auto start=[](Component component) {
        std::thread([component]{
            pthread_setname_np(pthread_self(),component.name);
            int status=component.run();
            fprintf(stderr,"POCKET_CORE_COMPONENT_EXIT name=%s status=%d\n",component.name,status);
            // A partially alive core must not continue advertising old services.
            _exit(70);
        }).detach();
    };
    for(const auto& component:components)start(component);
    if(sharedMetrics)start({"pocket-metrics",pocketStartMediaMetricsInCore32});
#ifdef POCKET_CORE_INSTALLD
    if(sharedInstalld)start({"pocket-installd",pocketStartInstalldInCore32});
#endif
    IPCThreadState::self()->joinThreadPool();return 3;
}
