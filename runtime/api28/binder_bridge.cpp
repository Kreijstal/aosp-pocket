// SPDX-License-Identifier: GPL-3.0-only
// Android 9 platform ABI bridge. Deliberately separate from Ananbox's API 25
// HostBinder class ABI. Host Parcel templates come from the running host API.
#if POCKET_GUEST_API != 28
#error This bridge requires the Android 9 platform Binder ABI
#endif
#include "binder_bridge.h"
#include <binder/Binder.h>
#include <binder/IPCThreadState.h>
#include <binder/Parcel.h>
#include <binder/ProcessState.h>
#include <utils/String16.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

using namespace android;
namespace {
bool readAll(FILE* stream, void* data, size_t size) {
    return fread(data, 1, size, stream) == size;
}

status_t loadTemplate(const char* path, Parcel& parcel, const sp<IBinder>& binder) {
    FILE* stream = fopen(path, "rb");
    if (!stream) return -errno;
    uint64_t sizes[2] = {};
    if (!readAll(stream, sizes, sizeof(sizes)) || sizes[0] > 65536 || sizes[0] % 4 || sizes[1] > 1) {
        fclose(stream);
        return BAD_VALUE;
    }
    const size_t length = sizes[0] + sizes[1] * sizeof(binder_size_t);
    auto* bytes = static_cast<uint8_t*>(malloc(length));
    if (!bytes) { fclose(stream); return NO_MEMORY; }
    bool valid = readAll(stream, bytes, length) && fgetc(stream) == EOF;
    fclose(stream);
    // Parcel data is 4-byte aligned; the separately stored offsets require
    // 8-byte alignment on ARM64. Copy instead of casting an unaligned pointer.
    binder_size_t offset = 0;
    if (sizes[1]) {
        memcpy(&offset, bytes + sizes[0], sizeof(offset));
        if (offset % 4 || sizes[0] < sizeof(flat_binder_object) || offset > sizes[0] - sizeof(flat_binder_object))
            valid = false;
        if (valid) {
            flat_binder_object object;
            memcpy(&object, bytes + offset, sizeof(object));
            if (!binder || !binder->localBinder() || object.hdr.type != BINDER_TYPE_BINDER)
                valid = false;
        }
    } else if (binder) valid = false;
    if (!valid) { free(bytes); return BAD_VALUE; }
    // Rebuild through the public Parcel API so it owns the Binder reference.
    // The host's bytes after the flat object include any newer stability trailer.
    status_t result;
    if (sizes[1]) {
        result = parcel.write(bytes, offset);
        if (result == NO_ERROR) result = parcel.writeStrongBinder(binder);
        const size_t end = offset + sizeof(flat_binder_object);
        if (result == NO_ERROR) result = parcel.write(bytes + end, sizes[0] - end);
    } else result = parcel.setData(bytes, sizes[0]);
    free(bytes);
    if (result != NO_ERROR) return result;
    return NO_ERROR;
}

int readNumber(const char* path) {
    FILE* stream = fopen(path, "r");
    if (!stream) return -1;
    int number = -1;
    if (fscanf(stream, "%d", &number) != 1) number = -1;
    fclose(stream);
    return number;
}

sp<IBinder> hostActivity() {
    Parcel request, reply;
    if (loadTemplate("/activityServiceRequest", request, nullptr) != NO_ERROR) return nullptr;
    // Bootstrap against host handle zero explicitly, avoiding the guest
    // getContextObject interposer and its recursion into itself.
    status_t result = IPCThreadState::self()->transact(0, 1, request, &reply, 0);
    if (result != NO_ERROR) return nullptr;
    int hostApi = readNumber("/host-api");
    if (hostApi < 28) return nullptr;
    // ServiceManager became AIDL in Android 11. Older native SM replies begin
    // directly with the Binder object, without an AIDL exception header.
    if (hostApi >= 30 && reply.readExceptionCode() != 0) return nullptr;
    return reply.readStrongBinder(); // Any newer host stability trailer is last.
}

status_t broadcast(const sp<IBinder>& activity, const char* path, const sp<IBinder>& object) {
    int transaction = readNumber("/trans_code");
    if (!activity || transaction <= 0) return BAD_VALUE;
    Parcel request, reply;
    status_t result = loadTemplate(path, request, object);
    if (result != NO_ERROR) return result;
    result = activity->transact(transaction, request, &reply, 0);
    if (result != NO_ERROR) return result;
    return reply.readExceptionCode();
}

class Receiver final : public BBinder {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    sp<IBinder> manager;
public:
    ~Receiver() override { pthread_cond_destroy(&condition); pthread_mutex_destroy(&mutex); }
    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
        if (code != 2) return BBinder::onTransact(code, data, reply, flags);
        int position = readNumber("/host-token-position");
        if (position < 0 || static_cast<size_t>(position) >= data.dataSize()) return BAD_VALUE;
        data.setDataPosition(position);
        if (data.readString16() != String16("com.github.ananbox.ILocalInterface")) return PERMISSION_DENIED;
        sp<IBinder> value = data.readStrongBinder();
        if (!value) return BAD_VALUE;
        pthread_mutex_lock(&mutex);
        manager = value;
        pthread_cond_signal(&condition);
        pthread_mutex_unlock(&mutex);
        if (reply) reply->writeInt32(0);
        return NO_ERROR;
    }
    sp<IBinder> wait() {
        timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        ++deadline.tv_sec;
        pthread_mutex_lock(&mutex);
        while (!manager) {
            int result = pthread_cond_timedwait(&condition, &mutex, &deadline);
            if (result != 0) break;
        }
        sp<IBinder> value = manager;
        pthread_mutex_unlock(&mutex);
        return value;
    }
};
}

namespace pocket {
status_t publishManager(const sp<IBinder>& manager) {
    if (!manager) return BAD_VALUE;
    ProcessState::self()->startThreadPool();
    return broadcast(hostActivity(), "/binderBroadcastIntent", manager);
}
}

namespace android {
sp<IBinder> ProcessState::getContextObject(const sp<IBinder>&) {
    // Never silently direct a vendor Binder request to the host/framework SM.
    if (strcmp(getDriverName().c_str(), "/dev/binder") != 0) return nullptr;
    sp<IBinder> activity = hostActivity();
    if (!activity) return nullptr;
    sp<Receiver> receiver = new Receiver;
    startThreadPool();
    for (int attempt = 0; attempt < 15; ++attempt) {
        if (broadcast(activity, "/localBroadcastIntent", receiver) != NO_ERROR) return nullptr;
        sp<IBinder> manager = receiver->wait();
        if (manager) return manager;
    }
    return nullptr;
}

sp<IBinder> ProcessState::getContextObject(const String16& name, const sp<IBinder>& caller) {
    return name == String16("default") ? getContextObject(caller) : nullptr;
}
}
