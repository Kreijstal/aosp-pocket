// SPDX-License-Identifier: GPL-3.0-only
// Android 10's pinned ISuspendControlService AIDL order; host power policy owns suspend.
#include <binder/Binder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <binder/ProcessState.h>
#include <stdio.h>
#include <unistd.h>
using namespace android;
class SuspendControl final : public BBinder {
    status_t onTransact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags) override {
        if(code<1 || code>3)return BBinder::onTransact(code,data,reply,flags);
        uid_t uid=IPCThreadState::self()->getCallingUid();
        if(uid!=0 && uid!=1000)return PERMISSION_DENIED;
        if(!reply || !data.enforceInterface(String16("android.system.suspend.ISuspendControlService")))return BAD_VALUE;
        // enableAutosuspend, registerCallback, forceSuspend are unavailable
        // to an ordinary app. In particular, never suspend the host kernel.
        if(code==2)data.readStrongBinder();
        reply->writeInt32(0);reply->writeInt32(0);
        fprintf(stderr,"API29_SUSPEND_CONTROL operation=%u supported=0 host_policy=retained\n",code);
        return NO_ERROR;
    }
};
int main() {
    setbuf(stdout,nullptr);if(getuid()!=0)return 64;
    ProcessState::self()->startThreadPool();
    auto manager=defaultServiceManager();
    if(!manager || manager->addService(String16("suspend_control"),new SuspendControl)!=NO_ERROR)return 1;
    puts("API29_SUSPEND_CONTROL_READY");
    IPCThreadState::self()->joinThreadPool();return 0;
}
