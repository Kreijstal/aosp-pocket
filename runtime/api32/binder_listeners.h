// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <binder/IBinder.h>
#include <binder/IInterface.h>
#include <binder/Status.h>
#include <mutex>
#include <vector>
#include <errno.h>

template<class Interface> class BinderListeners32 final : public android::RefBase {
    struct Entry {android::sp<Interface> service;android::sp<android::IBinder::DeathRecipient> death;};
    std::mutex mutex;
    std::vector<Entry> entries;
    class Death final : public android::IBinder::DeathRecipient {
        android::wp<BinderListeners32> owner;
    public:
        explicit Death(BinderListeners32* value):owner(value){}
        void binderDied(const android::wp<android::IBinder>&) override {
            auto registry=owner.promote();if(!registry)return;
            std::lock_guard<std::mutex> lock(registry->mutex);
            for(auto i=registry->entries.begin();i!=registry->entries.end();++i)
                if(i->death.get()==this){registry->entries.erase(i);break;}
        }
    };
public:
    android::binder::Status add(const android::sp<Interface>& service) {
        using android::binder::Status;
        if(!service)return Status::fromServiceSpecificError(EINVAL,"A typed listener is required");
        auto binder=android::IInterface::asBinder(service);
        if(!binder || binder->pingBinder()!=android::NO_ERROR)
            return Status::fromServiceSpecificError(EINVAL,"Listener is not live");
        std::lock_guard<std::mutex> lock(mutex);
        for(const auto& entry:entries)if(android::IInterface::asBinder(entry.service)==binder)return Status::ok();
        if(entries.size()>=32)return Status::fromServiceSpecificError(ENOSPC,"Too many listeners");
        android::sp<android::IBinder::DeathRecipient> death=new Death(this);
        auto status=binder->linkToDeath(death);
        if(status!=android::NO_ERROR && status!=android::INVALID_OPERATION)
            return Status::fromServiceSpecificError(EPIPE,"Listener exited during registration");
        entries.push_back({service,death});return Status::ok();
    }
    std::vector<android::sp<Interface>> snapshot() {
        std::lock_guard<std::mutex> lock(mutex);std::vector<android::sp<Interface>> result;
        for(const auto& entry:entries)result.push_back(entry.service);
        return result;
    }
};
