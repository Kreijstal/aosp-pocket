// SPDX-License-Identifier: GPL-3.0-only
// 12L's legacy C++ getService polls checkService, which cannot activate lazy
// services. Ask the typed manager once, then retain its stock bounded wait.
#include <android/os/IServiceManager.h>
#include <binder/IServiceManager.h>
#include <utils/String8.h>
#include <dlfcn.h>
#include "lazy_service_runtime.h"
namespace android {
using AidlServiceManager=android::os::IServiceManager;
// Copied from the pinned platform source at build time, with its license.
#include "pocket_service_manager_shim32.h"
sp<IBinder> ServiceManagerShim::getService(const String16& name) const {
    using Original=sp<IBinder>(*)(const ServiceManagerShim*,const String16&);
    static auto original=reinterpret_cast<Original>(dlsym(RTLD_NEXT,"_ZNK7android18ServiceManagerShim10getServiceERKNS_8String16E"));
    if(!original)return nullptr;
    String8 utf8(name);
    if(pocket::lazyService32(utf8.c_str()) && pocket::lazyMediaEnabled32()) {
        timespec before{},after{};clock_gettime(CLOCK_MONOTONIC,&before);
        sp<IBinder> service;
        auto status=mTheRealServiceManager->getService(utf8.c_str(),&service);
        if(!status.isOk())return nullptr;
        if(service)return service;
        service=original(this,name);clock_gettime(CLOCK_MONOTONIC,&after);
        const auto elapsed=(after.tv_sec-before.tv_sec)*1000+(after.tv_nsec-before.tv_nsec)/1000000;
        fprintf(stderr,"API32_LAZY_BINDER_WAIT name=%s ready=%d elapsed_ms=%lld\n",utf8.c_str(),service!=nullptr,static_cast<long long>(elapsed));
        return service;
    }
    return original(this,name);
}
}
