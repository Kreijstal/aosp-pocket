// SPDX-License-Identifier: GPL-3.0-only
// Experimental source-level interposer for the Ananbox Android 7.1 guest.
// Its original getSVMObj mixes a Binder thread pool with blocking polling on
// the caller thread. A pool thread can consume the callback, leaving that
// caller asleep forever. Deliver on a pool thread and wait on a condition.
#include <binder/Binder.h>
#include <binder/HostBinder.h>
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <binder/Parcel.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

namespace android {
namespace {
class BootstrapReceiver final : public LocalBinder {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    HostBinderShim* host;
public:
    explicit BootstrapReceiver(std::shared_ptr<HostBinderShim>& shim)
        : LocalBinder(shim), host(shim.get()) {}
    ~BootstrapReceiver() override {
        pthread_cond_destroy(&condition);
        pthread_mutex_destroy(&mutex);
    }
    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
        if (code != 2) return LocalBinder::onTransact(code, data, reply, flags);
        host->enforceDescriptor(data);
        sp<IBinder> received = data.readStrongBinder();
        pthread_mutex_lock(&mutex);
        remoteBinder = received;
        pthread_cond_signal(&condition);
        pthread_mutex_unlock(&mutex);
        return NO_ERROR;
    }
    sp<IBinder> wait() {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        ++deadline.tv_sec;
        pthread_mutex_lock(&mutex);
        if (remoteBinder == nullptr) pthread_cond_timedwait(&condition, &mutex, &deadline);
        sp<IBinder> result = remoteBinder;
        pthread_mutex_unlock(&mutex);
        return result;
    }
};
}

sp<IBinder> HostBinder::getSVMObj() {
    sp<IBinder> activity = shim->getHostAMS();
    if (activity == nullptr) return nullptr;
    sp<BootstrapReceiver> receiver = new BootstrapReceiver(shim);
    ProcessState::self()->startThreadPool();
    const int transaction = getTransCode();
    for (;;) {
        Parcel request;
        shim->broadCastIntent(request, "local", receiver);
        if (transaction == 0) shim->sendBroadCast(activity, request);
        else activity->transact(transaction, request, nullptr, 0);
        sp<IBinder> manager = receiver->wait();
        if (manager != nullptr) return manager;
    }
}
}
