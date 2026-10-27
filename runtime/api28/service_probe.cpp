// SPDX-License-Identifier: GPL-3.0-only
// Two independent Android 9 processes: registry/server and Binder client.
#include "binder_bridge.h"
#include "binder_token.h"
#include <binder/Binder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <binder/ProcessState.h>
#include <utils/String8.h>
#include <map>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/system_properties.h>
#ifndef POCKET_GUEST_API
#define POCKET_GUEST_API 28
#endif
#define POCKET_API_STRING_INNER(value) #value
#define POCKET_API_STRING(value) POCKET_API_STRING_INNER(value)
#define POCKET_API_LABEL "API" POCKET_API_STRING(POCKET_GUEST_API)
#define POCKET_ECHO_SERVICE "pocket.api" POCKET_API_STRING(POCKET_GUEST_API) ".echo"
#define POCKET_FD_VALUE "api" POCKET_API_STRING(POCKET_GUEST_API) "fd"

using namespace android;
namespace {
class Echo final : public BBinder {
    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
        if (code != FIRST_CALL_TRANSACTION) return BBinder::onTransact(code, data, reply, flags);
        if (!data.enforceInterface(String16("dev.kreijstal.pocket.Echo"))) return PERMISSION_DENIED;
        int32_t input = data.readInt32();
        int fd = data.readFileDescriptor();
        char bytes[8] = {};
        if (fd < 0 || pread(fd, bytes, 7, 0) != 7 || strcmp(bytes, POCKET_FD_VALUE)) return BAD_VALUE;
        reply->writeInt32(input + 1);
        reply->writeInt32(getpid());
        reply->writeInt32(getuid());
        reply->writeInt32(IPCThreadState::self()->getCallingUid());
        reply->writeInt32(IPCThreadState::self()->getCallingPid());
#if POCKET_GUEST_API == 29
        reply->writeInt32(IPCThreadState::self()->getCallingWorkSourceUid());
        reply->writeInt32(const_cast<Parcel&>(data).readCallingWorkSourceUid());
#endif
        return NO_ERROR;
    }
};

// Minimum API 28 IServiceManager wire protocol for bring-up, not a complete
// production manager. No host BINDER_SET_CONTEXT_MGR or global registration.
class Registry final : public BBinder {
    struct Entry { sp<IBinder> binder; sp<IBinder::DeathRecipient> death; };
    std::map<String16, Entry> services;
    Mutex mutex;
    class ServiceDeath final : public IBinder::DeathRecipient {
        wp<Registry> owner;
        String16 name;
    public:
        ServiceDeath(Registry* registry,const String16& service):owner(registry),name(service) {}
        void binderDied(const wp<IBinder>&) override {
            sp<Registry> registry=owner.promote();
            if (registry) registry->removeDead(name,this);
        }
    };
    void removeDead(const String16& name,const IBinder::DeathRecipient* recipient) {
        Mutex::Autolock guard(mutex);
        auto entry=services.find(name);
        if (entry!=services.end() && entry->second.death.get()==recipient) {
            printf(POCKET_API_LABEL "_REGISTRY_REMOVE_DEAD name=%s\n",String8(name).c_str());
            services.erase(entry);
        }
    }
    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
        if (code < 1 || code > 4) return BBinder::onTransact(code, data, reply, flags);
        PocketBinderToken header={};
        bool modern=pocket_binder_token(data.data(),data.dataSize(),&header) && header.format==2;
        if (!data.enforceInterface(String16("android.os.IServiceManager"))) return PERMISSION_DENIED;
        Mutex::Autolock guard(mutex);
        if (code == IServiceManager::GET_SERVICE_TRANSACTION || code == IServiceManager::CHECK_SERVICE_TRANSACTION) {
            auto entry = services.find(data.readString16());
            if (modern) reply->writeInt32(0); // AIDL ServiceManager exception header.
            return reply->writeStrongBinder(entry == services.end() ? nullptr : entry->second.binder);
        }
        if (code == IServiceManager::ADD_SERVICE_TRANSACTION) {
            String16 name = data.readString16();
            sp<IBinder> service = data.readStrongBinder();
            if (!service || !name.size()) return BAD_VALUE;
            sp<IBinder::DeathRecipient> death=new ServiceDeath(this,name);
            status_t result=service->linkToDeath(death);
            // Locally owned Binder objects live with this registry process.
            if (result!=NO_ERROR && result!=INVALID_OPERATION) return result;
            auto previous=services.find(name);
            if (previous!=services.end()) previous->second.binder->unlinkToDeath(previous->second.death);
            services[name] = {service,death};
            return reply->writeInt32(0);
        }
        int index = data.readInt32();
        if (index < 0 || static_cast<size_t>(index) >= services.size()) return BAD_VALUE;
        auto entry = services.begin();
        std::advance(entry, index);
        return reply->writeString16(entry->first);
    }
};
}

int main(int argc, char** argv) {
    setbuf(stdout, nullptr);
    if (argc != 2) return 64;
    char sdk[PROP_VALUE_MAX];
    __system_property_get("ro.build.version.sdk", sdk);
    printf(POCKET_API_LABEL "_START mode=%s pid=%d uid=%d sdk=%s\n", argv[1], getpid(), getuid(), sdk);
    if (strcmp(sdk, POCKET_API_STRING(POCKET_GUEST_API))) return 65;
#if POCKET_GUEST_API == 29
    if (!strcmp(argv[1], "client-virtual") && (setgid(39999) || setuid(39999))) return 66;
#endif
    ProcessState::self()->startThreadPool();
    if (!strcmp(argv[1], "manager")) {
        sp<IBinder> manager = new Registry;
        status_t result = pocket::publishManager(manager);
        printf(POCKET_API_LABEL "_PUBLISH status=%d\n", result);
        if (result != NO_ERROR) return 1;
        // The normal API 28 client must discover this published registry.
        sp<IServiceManager> sm = defaultServiceManager();
        if (!sm || sm->addService(String16(POCKET_ECHO_SERVICE), new Echo) != NO_ERROR) return 2;
        puts(POCKET_API_LABEL "_READY");
        IPCThreadState::self()->joinThreadPool();
        return 0;
    }
    const bool virtualClient = !strcmp(argv[1], "client-virtual");
    if (strcmp(argv[1], "client") && !virtualClient) return 64;
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> echo = sm ? sm->checkService(String16(POCKET_ECHO_SERVICE)) : nullptr;
    if (!echo) return 3;
    Parcel request, reply;
#if POCKET_GUEST_API == 29
    const int64_t workSourceToken=IPCThreadState::self()->setCallingWorkSourceUid(29017);
#endif
    request.writeInterfaceToken(String16("dev.kreijstal.pocket.Echo"));
#if POCKET_GUEST_API == 29
    if (request.readCallingWorkSourceUid()!=29017 || !request.replaceCallingWorkSourceUid(29018) ||
        request.readCallingWorkSourceUid()!=29018) return 7;
    IPCThreadState::self()->restoreCallingWorkSource(workSourceToken);
#endif
    request.writeInt32(POCKET_GUEST_API-1);
    FILE* file = tmpfile();
    if (!file) return 4;
    if (fwrite(POCKET_FD_VALUE, 1, 7, file) != 7 || fflush(file)) return 5;
    request.writeFileDescriptor(fileno(file));
    status_t result = echo->transact(IBinder::FIRST_CALL_TRANSACTION, request, &reply);
    int answer = reply.readInt32(), server = reply.readInt32(), uid = reply.readInt32();
    int callerUid = reply.readInt32(), callerPid = reply.readInt32();
#if POCKET_GUEST_API == 29
    int workSource=reply.readInt32(), parcelWorkSource=reply.readInt32();
    printf("API29_WORKSOURCE thread_uid=%d parcel_uid=%d\n",workSource,parcelWorkSource);
    if (workSource!=29018 || parcelWorkSource!=29018) return 8;
#endif
    fclose(file);
    printf(POCKET_API_LABEL "_RESULT status=%d answer=%d server_pid=%d client_pid=%d server_uid=%d client_uid=%d fd=" POCKET_FD_VALUE " caller_uid=%d caller_pid=%d\n",
           result, answer, server, getpid(), uid, getuid(), callerUid, callerPid);
    return result == NO_ERROR && answer == POCKET_GUEST_API && server != getpid() &&
        (virtualClient ? uid == 0 && getuid() == 39999 : static_cast<uid_t>(uid) == getuid())
        && static_cast<uid_t>(callerUid) == getuid() && callerPid == getpid() ? 0 : 6;
}
