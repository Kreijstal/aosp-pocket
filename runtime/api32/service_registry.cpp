// SPDX-License-Identifier: GPL-3.0-only
// Generated API 32 BnServiceManager handles the stock SYST token, AIDL status
// headers, Binder stability words and typed vectors. No API 28 wire emulation.
#include "service_registry.h"
#include "lazy_service_runtime.h"
#include <android/os/BnServiceManager.h>
#include <android/os/IServiceCallback.h>
#include <binder/IPCThreadState.h>
#include <utils/Mutex.h>
#include <algorithm>
#include <map>
#include <stdio.h>
#include <sys/system_properties.h>
#include <string.h>

using namespace android;
using android::binder::Status;
namespace pocket {
namespace {
bool validName(const std::string& name) {
    if (name.empty() || name.size() > 127) return false;
    for (unsigned char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/')) return false;
    return true;
}
bool softwareKeystoreEnabled() {
    char value[PROP_VALUE_MAX];__system_property_get("ro.pocket.keystore.enabled",value);
    return !strcmp(value,"true") || !strcmp(value,"1");
}

class Registry final : public os::BnServiceManager {
    struct Entry {
        sp<IBinder> binder;
        sp<IBinder::DeathRecipient> death;
        bool allowIsolated;
        int32_t priority, pid;
    };
    std::map<std::string, Entry> services;
    std::map<std::string, std::vector<sp<os::IServiceCallback>>> notifications;
    Mutex mutex;
    class ServiceDeath final : public IBinder::DeathRecipient {
        wp<Registry> owner;
        std::string name;
    public:
        ServiceDeath(Registry* registry, const std::string& service) : owner(registry), name(service) {}
        void binderDied(const wp<IBinder>&) override {
            sp<Registry> registry = owner.promote();
            if (registry) registry->removeDead(name, this);
        }
    };
    void removeDead(const std::string& name, const IBinder::DeathRecipient* recipient) {
        Mutex::Autolock lock(mutex);
        auto found = services.find(name);
        if (found != services.end() && found->second.death.get() == recipient) {
            printf("API32_REGISTRY_REMOVE_DEAD name=%s\n", name.c_str());
            services.erase(found);
        }
    }
    static Status unsupported(const char* message) {
        return Status::fromExceptionCode(Status::EX_UNSUPPORTED_OPERATION, message);
    }
public:
    Status getService(const std::string& name, sp<IBinder>* result) override {
        auto status=checkService(name,result);
        if(status.isOk() && !*result && IPCThreadState::self()->getCallingUid()%100000<99000)
            requestLazyService32(name.c_str());
        return status;
    }
    Status checkService(const std::string& name, sp<IBinder>* result) override {
        Mutex::Autolock lock(mutex);
        result->clear();
        auto found = services.find(name);
        const uid_t uid = IPCThreadState::self()->getCallingUid() % 100000;
        if (found != services.end() && (uid < 99000 || found->second.allowIsolated))
            *result = found->second.binder;
        return Status::ok();
    }
    Status addService(const std::string& name, const sp<IBinder>& service, bool allowIsolated, int32_t priority) override {
        if (!validName(name) || !service) return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT);
        if (IPCThreadState::self()->getCallingUid() % 100000 >= 10000)
            return Status::fromExceptionCode(Status::EX_SECURITY);
        sp<IBinder::DeathRecipient> death = new ServiceDeath(this, name);
        status_t status = service->linkToDeath(death);
        if (status != NO_ERROR && status != INVALID_OPERATION) return Status::fromStatusT(status);
        std::vector<sp<os::IServiceCallback>> callbacks;
        {
            Mutex::Autolock lock(mutex);
            auto found = services.find(name);
            if (found != services.end()) found->second.binder->unlinkToDeath(found->second.death);
            services[name] = {service, death, allowIsolated, priority, IPCThreadState::self()->getCallingPid()};
            if(lazyService32(name.c_str()) && lazyMediaEnabled32()) {
                timespec now{};clock_gettime(CLOCK_MONOTONIC,&now);
                printf("API32_LAZY_SERVICE_PUBLISHED name=%s pid=%d monotonic_ms=%lld\n",name.c_str(),
                    IPCThreadState::self()->getCallingPid(),static_cast<long long>(now.tv_sec)*1000+now.tv_nsec/1000000);
            }
            auto waiting = notifications.find(name);
            if (waiting != notifications.end()) callbacks = waiting->second;
        }
        // Never call a client's Binder while holding the registry mutex.
        for (const auto& callback : callbacks) callback->onRegistration(name, service);
        return Status::ok();
    }
    Status listServices(int32_t priority, std::vector<std::string>* result) override {
        Mutex::Autolock lock(mutex);
        result->clear();
        for (const auto& [name, entry] : services)
            if (entry.priority & priority) result->push_back(name);
        return Status::ok();
    }
    Status registerForNotifications(const std::string& name, const sp<os::IServiceCallback>& callback) override {
        if (!validName(name) || !callback) return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT);
        // Isolated processes may only discover services explicitly allowing
        // them; don't let a notification bypass that lookup restriction.
        if (IPCThreadState::self()->getCallingUid() % 100000 >= 99000)
            return Status::fromExceptionCode(Status::EX_SECURITY);
        sp<IBinder> existing;
        {
            Mutex::Autolock lock(mutex);
            auto& callbacks = notifications[name];
            auto same = [&](const auto& value) { return IInterface::asBinder(value) == IInterface::asBinder(callback); };
            if (std::none_of(callbacks.begin(), callbacks.end(), same)) callbacks.push_back(callback);
            auto found = services.find(name);
            if (found != services.end()) existing = found->second.binder;
        }
        if (existing) callback->onRegistration(name, existing);
        return Status::ok();
    }
    Status unregisterForNotifications(const std::string& name, const sp<os::IServiceCallback>& callback) override {
        if (!callback) return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT);
        Mutex::Autolock lock(mutex);
        auto found = notifications.find(name);
        if (found == notifications.end()) return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE);
        auto& values = found->second;
        auto end = std::remove_if(values.begin(), values.end(), [&](const auto& value) {
            return IInterface::asBinder(value) == IInterface::asBinder(callback);
        });
        if (end == values.end()) return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE);
        values.erase(end, values.end());
        if (values.empty()) notifications.erase(found);
        return Status::ok();
    }
    Status isDeclared(const std::string& name, bool* result) override {
        *result = softwareKeystoreEnabled() && name=="android.system.keystore2.IKeystoreService/default";
        return Status::ok();
    }
    Status getDeclaredInstances(const std::string& name, std::vector<std::string>* result) override {
        result->clear();
        if(softwareKeystoreEnabled() && name=="android.system.keystore2.IKeystoreService")result->push_back("default");
        return Status::ok();
    }
    Status updatableViaApex(const std::string&, std::optional<std::string>* result) override {
        result->reset();
        return Status::ok();
    }
    Status registerClientCallback(const std::string&, const sp<IBinder>&, const sp<os::IClientCallback>&) override {
        return unsupported("Lazy service client counting is not implemented in the foundation profile");
    }
    Status tryUnregisterService(const std::string&, const sp<IBinder>&) override {
        // Keep a live service rather than inventing a kernel client count.
        return unsupported("Lazy service unregistration requires verified client counting");
    }
    Status getServiceDebugInfo(std::vector<os::ServiceDebugInfo>* result) override {
        Mutex::Autolock lock(mutex);
        result->clear();
        for (const auto& [name, entry] : services) {
            os::ServiceDebugInfo info;
            info.name = name;
            info.debugPid = entry.pid;
            result->push_back(std::move(info));
        }
        return Status::ok();
    }
};
}
sp<IBinder> createServiceRegistry32() { return new Registry; }
}
