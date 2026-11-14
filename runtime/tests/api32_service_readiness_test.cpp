// SPDX-License-Identifier: GPL-3.0-only
#include "../api32/service_readiness.h"
#include <assert.h>
#include <stdio.h>
#include <vector>

int main() {
    uint64_t now=0;std::vector<uint64_t> sleeps;
    auto clock=[&]{return now;};
    auto sleep=[&](uint64_t ms){assert(ms>0 && ms<=50);sleeps.push_back(ms);now+=ms;};
    auto immediate=pocket::awaitServiceReadiness(60000,50,clock,[]{return true;},sleep);
    assert(immediate.ready && immediate.elapsedMs==0 && immediate.checks==1 && sleeps.empty());
    auto late=[&]{return now>=12000;};
    auto old=pocket::awaitServiceReadiness(10000,50,clock,late,sleep);
    assert(!old.ready && old.elapsedMs==10000);
    now=0;sleeps.clear();
    auto cold=pocket::awaitServiceReadiness(60000,50,clock,late,sleep);
    assert(cold.ready && cold.elapsedMs==12000 && cold.checks==241);
    now=0;sleeps.clear();
    auto missing=pocket::awaitServiceReadiness(60000,50,clock,[]{return false;},sleep);
    assert(!missing.ready && missing.elapsedMs==60000 && missing.checks==1200 && sleeps.size()==1200);
    now=0;sleeps.clear();unsigned foundButUnresponsive=0;
    auto recovering=pocket::awaitServiceReadiness(60000,50,clock,[&]{++foundButUnresponsive;return now>=2000;},sleep);
    assert(recovering.ready && recovering.elapsedMs==2000 && foundButUnresponsive==41);
    now=0;sleeps.clear();
    auto overrun=pocket::awaitServiceReadiness(60000,50,clock,[&]{now+=60001;return true;},sleep);
    assert(!overrun.ready && overrun.elapsedMs==60001 && overrun.checks==1 && sleeps.empty());
    now=0;sleeps.clear();
    auto partial=pocket::awaitServiceReadiness(125,50,clock,[]{return false;},sleep);
    assert(!partial.ready && partial.elapsedMs==125 && sleeps==std::vector<uint64_t>({50,50,25}));
    now=0;sleeps.clear();
    auto atDeadline=pocket::awaitServiceReadiness(60000,50,clock,[&]{now=60000;return true;},sleep);
    assert(!atDeadline.ready && atDeadline.checks==1 && sleeps.empty());
    puts("API32_SERVICE_READINESS_TEST_PASS immediate=1 cold_beyond_10s=1 missing_bounded=1 ping_recovery=1 rpc_overrun_rejected=1 final_sleep_bounded=1 exact_deadline_rejected=1");
}
