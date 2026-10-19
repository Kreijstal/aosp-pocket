// SPDX-License-Identifier: GPL-3.0-only
// Two independent Android 9 processes: registry/server and Binder client.
#include "binder_bridge.h"
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

using namespace android;
namespace {
class Echo final : public BBinder {
    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
        if (code != FIRST_CALL_TRANSACTION) return BBinder::onTransact(code, data, reply, flags);
        if (!data.enforceInterface(String16("dev.kreijstal.pocket.Echo"))) return PERMISSION_DENIED;
        int32_t input = data.readInt32();
        int fd = data.readFileDescriptor();
        char bytes[8] = {};
        if (fd < 0 || pread(fd, bytes, 7, 0) != 7 || strcmp(bytes, "api28fd")) return BAD_VALUE;
        reply->writeInt32(input + 1);
        reply->writeInt32(getpid());
        reply->writeInt32(getuid());
        reply->writeInt32(IPCThreadState::self()->getCallingUid());
        reply->writeInt32(IPCThreadState::self()->getCallingPid());
        return NO_ERROR;
    }
};

// Minimum API 28 IServiceManager wire protocol for bring-up, not a complete
// production manager. No host BINDER_SET_CONTEXT_MGR or global registration.
class Registry final : public BBinder {
    std::map<String16, sp<IBinder>> services;
    Mutex mutex;
    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
        if (code < 1 || code > 4) return BBinder::onTransact(code, data, reply, flags);
        if (!data.enforceInterface(String16("android.os.IServiceManager"))) return PERMISSION_DENIED;
        Mutex::Autolock guard(mutex);
        if (code == IServiceManager::GET_SERVICE_TRANSACTION || code == IServiceManager::CHECK_SERVICE_TRANSACTION) {
            auto entry = services.find(data.readString16());
            return reply->writeStrongBinder(entry == services.end() ? nullptr : entry->second);
        }
        if (code == IServiceManager::ADD_SERVICE_TRANSACTION) {
            String16 name = data.readString16();
            sp<IBinder> service = data.readStrongBinder();
            if (!service || !name.size()) return BAD_VALUE;
            services[name] = service;
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
    printf("API28_START mode=%s pid=%d uid=%d sdk=%s\n", argv[1], getpid(), getuid(), sdk);
    if (strcmp(sdk, "28")) return 65;
    ProcessState::self()->startThreadPool();
    if (!strcmp(argv[1], "manager")) {
        sp<IBinder> manager = new Registry;
        status_t result = pocket::publishManager(manager);
        printf("API28_PUBLISH status=%d\n", result);
        if (result != NO_ERROR) return 1;
        // The normal API 28 client must discover this published registry.
        sp<IServiceManager> sm = defaultServiceManager();
        if (!sm || sm->addService(String16("pocket.api28.echo"), new Echo) != NO_ERROR) return 2;
        puts("API28_READY");
        IPCThreadState::self()->joinThreadPool();
        return 0;
    }
    const bool virtualClient = !strcmp(argv[1], "client-virtual");
    if (strcmp(argv[1], "client") && !virtualClient) return 64;
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> echo = sm ? sm->checkService(String16("pocket.api28.echo")) : nullptr;
    if (!echo) return 3;
    Parcel request, reply;
    request.writeInterfaceToken(String16("dev.kreijstal.pocket.Echo"));
    request.writeInt32(27);
    FILE* file = tmpfile();
    if (!file) return 4;
    if (fwrite("api28fd", 1, 7, file) != 7 || fflush(file)) return 5;
    request.writeFileDescriptor(fileno(file));
    status_t result = echo->transact(IBinder::FIRST_CALL_TRANSACTION, request, &reply);
    int answer = reply.readInt32(), server = reply.readInt32(), uid = reply.readInt32();
    int callerUid = reply.readInt32(), callerPid = reply.readInt32();
    fclose(file);
    printf("API28_RESULT status=%d answer=%d server_pid=%d client_pid=%d server_uid=%d client_uid=%d fd=api28fd caller_uid=%d caller_pid=%d\n",
           result, answer, server, getpid(), uid, getuid(), callerUid, callerPid);
    return result == NO_ERROR && answer == 28 && server != getpid() &&
        (virtualClient ? uid == 0 && getuid() == 39999 : static_cast<uid_t>(uid) == getuid())
        && static_cast<uid_t>(callerUid) == getuid() && callerPid == getpid() ? 0 : 6;
}
