// SPDX-License-Identifier: GPL-3.0-only
#include "../../api32/service_activation.h"
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <thread>
#include <vector>
int main() {
    using namespace pocket;
    assert(!lazyService32("media.player.evil") && !lazyService32("pocket-adbd") &&
           !lazyService32("android.hardware.media.c2@1.2::IComponentStore/default",true));
    auto* player=lazyService32("media.player");auto* alias=lazyService32("media.resource_manager");
    auto* metrics=lazyService32("media.metrics");assert(player && alias==player && metrics);
    auto* sharedMetrics=lazyService32("media.metrics",false,true);
    assert(sharedMetrics && sharedMetrics->slot==metrics->slot && !sharedMetrics->requiresMedia &&
           !strcmp(sharedMetrics->init,"pocket-registry") && !lazyService32("media.metrics",true,true));
    assert(lazyService32("android.hardware.media.c2@1.0::IComponentStore/software",true)==
           lazyService32("android.hardware.media.c2@1.2::IComponentStore/software",true));
    assert(lazyService32("android.frameworks.cameraservice.service@2.0::ICameraService/default",true)==
           lazyService32("media.camera"));
    LazyServiceStarter32 start;unsigned calls=0;
    auto record=[&](const char* target){assert(!strcmp(target,"pocket-mediaserver"));++calls;};
    assert(!start.request(nullptr,true,true,0,record));
    assert(!start.request(player,false,true,0,record));
    assert(!start.request(player,true,false,0,record));
    assert(start.request(player,true,true,0,record) && calls==1);
    assert(!start.request(alias,true,true,999,record));
    assert(start.request(alias,true,true,1000,record) && calls==2);
    assert(start.request(metrics,true,false,1000,[&](const char* target){
        assert(!strcmp(target,"pocket-mediametrics"));
        assert(!start.request(metrics,true,false,3000,record));
        assert(start.request(player,true,true,3000,record));
    }));
    assert(!start.request(sharedMetrics,true,false,1999,record));
    assert(start.request(sharedMetrics,true,false,2000,[](const char* target){assert(!strcmp(target,"pocket-registry"));}));
    LazyServiceStarter32 parallel;std::atomic<unsigned> entered{0};std::mutex mutex;
    std::condition_variable ready;bool release=false;
    std::thread owner([&]{assert(parallel.request(player,true,true,0,[&](const char*){
        entered++;std::unique_lock<std::mutex> lock(mutex);ready.notify_all();ready.wait(lock,[&]{return release;});
    }));});
    {std::unique_lock<std::mutex> lock(mutex);ready.wait(lock,[&]{return entered.load()==1;});}
    std::vector<std::thread> peers;
    for(unsigned i=0;i<24;i++)peers.emplace_back([&]{assert(!parallel.request(alias,true,true,2000,[&](const char*){entered++;}));});
    for(auto& peer:peers)peer.join();
    assert(entered==1);
    {std::lock_guard<std::mutex> lock(mutex);release=true;}ready.notify_all();owner.join();
    assert(parallel.request(alias,true,true,2000,[&](const char*){entered++;}) && entered==2);
    puts("SERVICE_ACTIVATION_PASS exact_allowlist=1 flags=1 aliases=1 shared_metrics=1 cooldown=1 reentrant=1 concurrent_start=1");
}
