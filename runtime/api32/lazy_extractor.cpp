// SPDX-License-Identifier: GPL-3.0-only
// Publish the stock extractor Binder before doing any plugin scan. Starting a
// competing worker from the constructor can still starve Binder publication.
// First data/type demand waits for the real, mutex-protected factory load.
#include <list>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <utils/Mutex.h>
#include <media/MediaExtractorPluginApi.h>
#include <media/stagefright/MediaExtractorFactory.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdlib.h>
#include "lazy_service_runtime.h"
namespace {
template<class T> T original(const char* name) {
    auto value=reinterpret_cast<T>(dlsym(RTLD_NEXT,name));
    if(!value){fprintf(stderr,"API32_EXTRACTOR_SYMBOL_MISSING name=%s\n",name);abort();}
    return value;
}
bool deferred() {return getuid()==1040 && pocket::lazyMediaEnabled32();}
void load() {
    static auto function=original<void(*)()>("_ZN7android21MediaExtractorFactory14LoadExtractorsEv");
    function();
}
void ensureLoaded() {
    static std::once_flag loaded;
    std::call_once(loaded,[]{
        timespec before{},after{};clock_gettime(CLOCK_MONOTONIC,&before);
        load();clock_gettime(CLOCK_MONOTONIC,&after);
        auto elapsed=(after.tv_sec-before.tv_sec)*1000+(after.tv_nsec-before.tv_nsec)/1000000;
        fprintf(stderr,"API32_EXTRACTOR_PLUGINS_READY first_demand=1 elapsed_ms=%lld\n",static_cast<long long>(elapsed));
    });
}
}
namespace android {
void MediaExtractorFactory::LoadExtractors() {
    if(!deferred()){load();return;}
    fputs("API32_EXTRACTOR_PLUGINS_DEFERRED until=data_or_type_request\n",stderr);
}
sp<IMediaExtractor> MediaExtractorFactory::CreateFromService(const sp<DataSource>& source,const char* mime) {
    if(deferred())ensureLoaded();
    using Function=sp<IMediaExtractor>(*)(const sp<DataSource>&,const char*);
    static auto function=original<Function>("_ZN7android21MediaExtractorFactory17CreateFromServiceERKNS_2spINS_10DataSourceEEEPKc");
    return function(source,mime);
}
std::vector<std::string> MediaExtractorFactory::getSupportedTypes() {
    if(deferred())ensureLoaded();
    static auto function=original<std::vector<std::string>(*)()>("_ZN7android21MediaExtractorFactory17getSupportedTypesEv");
    return function();
}
}
