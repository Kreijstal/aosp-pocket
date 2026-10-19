// SPDX-License-Identifier: GPL-3.0-only
// Native-service Health 2.0 relay of actual host Android BatteryManager readings.
#include "boot_health.h"
#include <android/hardware/health/2.0/IHealth.h>
#include <atomic>
#include <algorithm>
#include <limits.h>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>
using namespace android;
using namespace android::hardware;
using namespace android::hardware::health::V2_0;
using android::hardware::health::V1_0::BatteryHealth;
using android::hardware::health::V1_0::BatteryStatus;
namespace {
struct Snapshot {
    HealthInfo info{};
    bool available=false;
    int counter=INT_MIN,current=INT_MIN,average=INT_MIN;
    long long energy=LLONG_MIN,stamp=0;
};
Snapshot readHost() {
    Snapshot s;s.info.legacy.batteryStatus=BatteryStatus::UNKNOWN;s.info.legacy.batteryHealth=BatteryHealth::UNKNOWN;
    FILE* input=fopen("/pocket-host-battery","re");if (!input) return s;
    int present=0,plugged=0,status=1,health=1,level=-1,scale=0,voltage=0,temperature=0;
    char technology[64]={0};
    int count=fscanf(input,"POCKET_BATTERY_V1\n%lld %d %d %d %d %d %d %d %d %d %d %d %lld\n%63[^\n]",
        &s.stamp,&present,&plugged,&status,&health,&level,&scale,&voltage,&temperature,
        &s.counter,&s.current,&s.average,&s.energy,technology);fclose(input);
    timespec now{};clock_gettime(CLOCK_BOOTTIME,&now);long long age=now.tv_sec*1000LL+now.tv_nsec/1000000-s.stamp;
    if (count<13 || age<0 || age>5000 || level<0 || scale<=0 || level>scale) return s;
    s.available=true;
    auto& legacy=s.info.legacy;
    legacy.batteryPresent=present!=0;legacy.chargerAcOnline=plugged&1;
    legacy.chargerUsbOnline=plugged&2;legacy.chargerWirelessOnline=plugged&4;
    if (status>=1 && status<=5) legacy.batteryStatus=static_cast<BatteryStatus>(status);
    if (health>=1 && health<=7) legacy.batteryHealth=static_cast<BatteryHealth>(health);
    legacy.batteryLevel=static_cast<int>(100LL*level/scale);
    legacy.batteryVoltage=voltage;legacy.batteryTemperature=temperature;legacy.batteryTechnology=technology;
    if (s.counter!=INT_MIN) legacy.batteryChargeCounter=s.counter;
    if (s.current!=INT_MIN) legacy.batteryCurrent=s.current;
    if (s.average!=INT_MIN) s.info.batteryCurrentAverage=s.average;
    return s;
}
class HealthRelay final : public IHealth {
    std::mutex mutex;
    std::vector<sp<IHealthInfoCallback>> callbacks;
    std::atomic<bool> active{true};
    std::thread worker;
    void send(const Snapshot& s) {
        std::vector<sp<IHealthInfoCallback>> list;
        { std::lock_guard<std::mutex> guard(mutex);list=callbacks; }
        for (const auto& callback:list) {
            auto result=callback->healthInfoChanged(s.info);
            if (!result.isOk()) {
                fprintf(stderr,"DIRECT_HEALTH_CALLBACK_FAILED %s\n",result.description().c_str());
                std::lock_guard<std::mutex> guard(mutex);
                callbacks.erase(std::remove(callbacks.begin(),callbacks.end(),callback),callbacks.end());
            }
        }
    }
public:
    HealthRelay():worker([this]{
        long long last=-1;bool lastAvailable=false;
        while(active) {
            auto s=readHost();
            if(s.stamp!=last || s.available!=lastAvailable) {send(s);last=s.stamp;lastAvailable=s.available;}
            usleep(1000000);
        }
    }) {}
    ~HealthRelay() override {active=false;if(worker.joinable())worker.join();}
    Return<Result> registerCallback(const sp<IHealthInfoCallback>& callback) override {
        if (!callback) return Result::NOT_FOUND;
        { std::lock_guard<std::mutex> guard(mutex);callbacks.push_back(callback); }
        auto s=readHost();auto result=callback->healthInfoChanged(s.info);
        fprintf(stderr,"DIRECT_HEALTH_REGISTER source=host_android available=%d present=%d level=%d callback_ok=%d\n",
                s.available,s.info.legacy.batteryPresent,s.info.legacy.batteryLevel,result.isOk());
        return result.isOk()?Result::SUCCESS:Result::CALLBACK_DIED;
    }
    Return<Result> unregisterCallback(const sp<IHealthInfoCallback>& callback) override {
        std::lock_guard<std::mutex> guard(mutex);
        for (auto i=callbacks.begin();i!=callbacks.end();++i) if (*i==callback) {callbacks.erase(i);return Result::SUCCESS;}
        return Result::NOT_FOUND;
    }
    Return<Result> update() override {send(readHost());return Result::SUCCESS;}
    Return<void> getChargeCounter(getChargeCounter_cb cb) override {auto s=readHost();cb(s.available&&s.counter!=INT_MIN?Result::SUCCESS:Result::NOT_SUPPORTED,s.counter==INT_MIN?0:s.counter);return Void();}
    Return<void> getCurrentNow(getCurrentNow_cb cb) override {auto s=readHost();cb(s.available&&s.current!=INT_MIN?Result::SUCCESS:Result::NOT_SUPPORTED,s.current==INT_MIN?0:s.current);return Void();}
    Return<void> getCurrentAverage(getCurrentAverage_cb cb) override {auto s=readHost();cb(s.available&&s.average!=INT_MIN?Result::SUCCESS:Result::NOT_SUPPORTED,s.average==INT_MIN?0:s.average);return Void();}
    Return<void> getCapacity(getCapacity_cb cb) override {auto s=readHost();cb(s.available?Result::SUCCESS:Result::NOT_SUPPORTED,s.info.legacy.batteryLevel);return Void();}
    Return<void> getEnergyCounter(getEnergyCounter_cb cb) override {auto s=readHost();cb(s.available&&s.energy!=LLONG_MIN?Result::SUCCESS:Result::NOT_SUPPORTED,s.energy==LLONG_MIN?0:s.energy);return Void();}
    Return<void> getChargeStatus(getChargeStatus_cb cb) override {auto s=readHost();cb(s.available?Result::SUCCESS:Result::NOT_SUPPORTED,s.info.legacy.batteryStatus);return Void();}
    Return<void> getStorageInfo(getStorageInfo_cb cb) override {cb(Result::NOT_SUPPORTED,{});return Void();}
    Return<void> getDiskStats(getDiskStats_cb cb) override {cb(Result::NOT_SUPPORTED,{});return Void();}
    Return<void> getHealthInfo(getHealthInfo_cb cb) override {auto s=readHost();cb(s.available?Result::SUCCESS:Result::NOT_SUPPORTED,s.info);return Void();}
};
}
sp<IHealth> pocketHealthRelay() {
    static sp<HealthRelay> relay=new HealthRelay;
    return relay;
}
