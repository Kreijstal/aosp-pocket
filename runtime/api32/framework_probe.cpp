// SPDX-License-Identifier: GPL-3.0-only
// Observe the real framework. This client never writes boot-completion state.
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <sys/system_properties.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <string>

using namespace android;
namespace {
std::string property(const char* name) {
    char value[PROP_VALUE_MAX];__system_property_get(name,value);return value;
}
}
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);
    if(argc!=2 || getuid()!=0 || property("ro.build.version.sdk")!="32")return 64;
    if(!strcmp(argv[1],"prepare")) {
        if(property("sys.boot_completed")=="1" || __system_property_set("ctl.stop","pocket-zygote"))return 1;
        for(unsigned i=0;i<200;i++) {
            if(property("init.svc.pocket-zygote")=="stopped") {
                puts("API32_FRAMEWORK_PREPARED previous_zygote=stopped boot_completed=unset");return 0;
            }
            usleep(50000);
        }
        return 2;
    }
    bool surface=!strcmp(argv[1],"surface");
    if((!surface && strcmp(argv[1],"wait")) || setgid(1000) || setuid(1000))return 64;
    ProcessState::self()->startThreadPool();
    auto manager=defaultServiceManager();if(!manager)return 3;
    if(surface) {
        for(unsigned i=0;i<600;i++) {
            const auto state=property("init.svc.pocket-surfaceflinger");
            if(state!="running") {printf("API32_SURFACEFLINGER_EXIT state=%s\n",state.c_str());return 6;}
            auto service=manager->checkService(String16("SurfaceFlinger"));
            if(service && service->pingBinder()==NO_ERROR && service->getInterfaceDescriptor()==String16("android.ui.ISurfaceComposer")) {
                puts("API32_SURFACEFLINGER_READY stock_daemon=1 interface=android.ui.ISurfaceComposer");return 0;
            }
            usleep(50000);
        }
        puts("API32_SURFACEFLINGER_TIMEOUT");return 7;
    }
    const std::string initialPid=property("init.svc_debug_pid.pocket-framework");
    const char* names[]={"activity","package","window"};
    const char* descriptors[]={"android.app.IActivityManager","android.content.pm.IPackageManager","android.view.IWindowManager"};
    bool reported[3]={};
    for(unsigned round=0;round<1800;round++) {
        const auto state=property("init.svc.pocket-framework");
        if(state!="running" || property("init.svc_debug_pid.pocket-framework")!=initialPid) {
            printf("API32_FRAMEWORK_EXIT state=%s initial_pid=%s current_pid=%s\n",state.c_str(),initialPid.c_str(),
                property("init.svc_debug_pid.pocket-framework").c_str());return 4;
        }
        bool ready=true;
        for(unsigned i=0;i<3;i++) {
            auto service=manager->checkService(String16(names[i]));
            bool live=service && service->pingBinder()==NO_ERROR && service->getInterfaceDescriptor()==String16(descriptors[i]);
            if(live && !reported[i]) {
                printf("API32_FRAMEWORK_SERVICE name=%s descriptor=%s\n",names[i],descriptors[i]);reported[i]=true;
            }
            ready=ready && live;
        }
        if(ready && property("sys.boot_completed")=="1") {
            puts("API32_FRAMEWORK_BOOT_PASS stock_system_server=1 boot_completed=1 activity_package_window=live");return 0;
        }
        usleep(100000);
    }
    printf("API32_FRAMEWORK_TIMEOUT boot_completed=%s activity=%d package=%d window=%d\n",
        property("sys.boot_completed").c_str(),reported[0],reported[1],reported[2]);return 5;
}
