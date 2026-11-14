// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

namespace pocket {
struct ServiceReadinessResult {
    bool ready;
    uint64_t elapsedMs;
    unsigned checks;
};

// Measure elapsed time, including Binder calls, rather than counting polls.
// DirectSession independently bounds the complete boot to 240 seconds.
template<class Clock, class Probe, class Sleep>
ServiceReadinessResult awaitServiceReadiness(uint64_t timeoutMs, uint64_t intervalMs,
                                            Clock clock, Probe probe, Sleep sleep) {
    const uint64_t started=clock();
    unsigned checks=0;
    for(;;) {
        uint64_t elapsed=clock()-started;
        if(elapsed>=timeoutMs || intervalMs==0)return {false,elapsed,checks};
        ++checks;
        const bool ready=probe();
        elapsed=clock()-started;
        // A late reply must not turn an expired deadline into success.
        if(elapsed>=timeoutMs)return {false,elapsed,checks};
        if(ready)return {true,elapsed,checks};
        const uint64_t remaining=timeoutMs-elapsed;
        sleep(remaining<intervalMs?remaining:intervalMs);
    }
}
}
