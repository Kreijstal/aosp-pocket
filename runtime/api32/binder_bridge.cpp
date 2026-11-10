#include "binder_host_scope.h"
// SPDX-License-Identifier: GPL-3.0-only
// Android 12L platform ABI bridge. Deliberately separate from Ananbox's API 25
// HostBinder class ABI. Host Parcel templates come from the running host API.
#if POCKET_GUEST_API != 32
#error Review this bridge against the matching platform Binder ABI
#endif
#include "binder_bridge.h"
#include "binder_stability.h"
#include <binder/Binder.h>
#include <binder/IPCThreadState.h>
#include <binder/Parcel.h>
#include <binder/ProcessState.h>
#include <utils/String16.h>
#include <utils/String8.h>
#include <linux/android/binder.h>
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
        fprintf(stderr,"API32_HOST_TEMPLATE_INVALID_HEADER path=%s bytes=%llu objects=%llu\n",path,
                static_cast<unsigned long long>(sizes[0]),static_cast<unsigned long long>(sizes[1]));
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
    if (!valid) { fprintf(stderr,"API32_HOST_TEMPLATE_INVALID_BODY path=%s\n",path);free(bytes); return BAD_VALUE; }
    // Rebuild through the public Parcel API so it owns the Binder reference.
    // The host's bytes after the flat object include any newer stability trailer.
    status_t result;
    if (sizes[1]) {
        result = parcel.write(bytes, offset);
        if (result == NO_ERROR) result = parcel.writeStrongBinder(binder);
        const size_t end = offset + sizeof(flat_binder_object);
        // API 32 writeStrongBinder appends its own four-byte stability category.
        // Host-generated templates already contain exactly the trailer expected
        // by that host API (and contain none on pre-Android-11 hosts).
        if (result == NO_ERROR && parcel.dataPosition() != end + sizeof(int32_t)) result = BAD_VALUE;
        if (result == NO_ERROR) result = parcel.setDataSize(end);
        if (result == NO_ERROR) parcel.setDataPosition(end);
        if (result == NO_ERROR) result = parcel.write(bytes + end, sizes[0] - end);
    } else result = parcel.setData(bytes, sizes[0]);
    free(bytes);
    if (result != NO_ERROR) { fprintf(stderr,"API32_HOST_TEMPLATE_REBUILD path=%s status=%d\n",path,result);return result; }
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

// API 32's short-lived Category encoding differs from both Android 11 and
// Android 13+. Copy the object through Parcel's reference-owning API and
// translate only its host stability trailer into the API 32 representation.
sp<IBinder> readHostBinder(const Parcel& source) {
    int hostApi=readNumber("/host-api");
    const size_t start=source.dataPosition();
    const size_t consumed=sizeof(flat_binder_object)+(hostApi>=30?sizeof(int32_t):0);
    if (source.dataAvail()<consumed) return nullptr;
    flat_binder_object object;
    memcpy(&object,source.data()+start,sizeof(object));
    if (object.hdr.type!=BINDER_TYPE_BINDER && object.hdr.type!=BINDER_TYPE_HANDLE) return nullptr;
    bool isNull=object.hdr.type==BINDER_TYPE_BINDER && !object.binder && !object.cookie;
    int32_t wire=0,category;
    if (hostApi>=30) memcpy(&wire,source.data()+start+sizeof(object),sizeof(wire));
    if (!pocket_api32_host_stability(hostApi,wire,isNull,&category)) {
        fprintf(stderr,"API32_HOST_STABILITY_UNSUPPORTED host=%d wire=%d\n",hostApi,wire);return nullptr;
    }
    Parcel compatible;
    if (compatible.appendFrom(&source,start,sizeof(object)) != NO_ERROR)
        return nullptr;
    compatible.setDataPosition(compatible.dataSize());
    if (compatible.writeInt32(category) != NO_ERROR) return nullptr;
    compatible.setDataPosition(0);
    sp<IBinder> value=compatible.readStrongBinder();
    source.setDataPosition(start+consumed);
    return value;
}

sp<IBinder> hostActivity() {
    PocketHostBinderScope hostScope;
    Parcel request, reply;
    if (loadTemplate("/activityServiceRequest", request, nullptr) != NO_ERROR) return nullptr;
    // Bootstrap against host handle zero explicitly, avoiding the guest
    // getContextObject interposer and its recursion into itself.
    status_t result = IPCThreadState::self()->transact(0, 1, request, &reply, 0);
    if (result != NO_ERROR) {fprintf(stderr,"API32_HOST_ACTIVITY_LOOKUP status=%d\n",result);return nullptr;}
    int hostApi = readNumber("/host-api");
    if (hostApi < 28) return nullptr;
    // ServiceManager became AIDL in Android 11. Older native SM replies begin
    // directly with the Binder object, without an AIDL exception header.
    if (hostApi >= 30 && reply.readExceptionCode() != 0) {fputs("API32_HOST_ACTIVITY_EXCEPTION\n",stderr);return nullptr;}
    sp<IBinder> activity=readHostBinder(reply);
    if (!activity) fprintf(stderr,"API32_HOST_ACTIVITY_BINDER_MISSING remaining=%zu error=%d\n",reply.dataAvail(),reply.errorCheck());
    return activity;
}

status_t broadcast(const sp<IBinder>& activity, const char* path, const sp<IBinder>& object) {
    PocketHostBinderScope hostScope;
    int transaction = readNumber("/trans_code");
    if (!activity || transaction <= 0) {fprintf(stderr,"API32_HOST_BROADCAST_MISSING activity=%d transaction=%d\n",!!activity,transaction);return BAD_VALUE;}
    Parcel request, reply;
    status_t result = loadTemplate(path, request, object);
    if (result != NO_ERROR) return result;
    result = activity->transact(transaction, request, &reply, 0);
    if (result != NO_ERROR) {fprintf(stderr,"API32_HOST_BROADCAST_TRANSACT status=%d\n",result);return result;}
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
        sp<IBinder> value = readHostBinder(data);
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

}
