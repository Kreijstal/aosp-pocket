// SPDX-License-Identifier: GPL-3.0-only
#include <binder/IServiceManager.h>
#include <binder/Binder.h>
#include <binder/IPCThreadState.h>
#include <binder/Parcel.h>
#include <binder/ProcessState.h>
#include <string.h>
#include <binder/IBinder.h>
#include <utils/String16.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/system_properties.h>
using namespace android;
class EarlyIdentity final : public BBinder {
    status_t onTransact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags) override {
        if (code!=FIRST_CALL_TRANSACTION) return BBinder::onTransact(code,data,reply,flags);
        // Deliberately check before enforceInterface, matching SurfaceFlinger.
        int uid=IPCThreadState::self()->getCallingUid();
        if (uid!=1000) return PERMISSION_DENIED;
        if (!data.enforceInterface(String16("dev.kreijstal.pocket.EarlyIdentity"))) return BAD_VALUE;
        reply->writeInt32(uid);return NO_ERROR;
    }
};
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);
    if(argc==2 && !strcmp(argv[1],"boot-status")) {
        char value[PROP_VALUE_MAX]={0};__system_property_get("sys.boot_completed",value);
        printf("DIRECT_BOOT_STATUS boot_completed=%s\n",value[0]?value:"0");return 0;
    }
    sp<IServiceManager> manager=defaultServiceManager();
    if (!manager) return 1;
    if (argc==2 && !strcmp(argv[1],"identity-server")) {
        ProcessState::self()->startThreadPool();
        if (manager->addService(String16("pocket.boot.early-identity"),new EarlyIdentity)!=NO_ERROR) return 3;
        puts("DIRECT_EARLY_IDENTITY_READY");IPCThreadState::self()->joinThreadPool();return 0;
    }
    if (argc==2 && !strcmp(argv[1],"identity-gone")) {
        for (int i=0;i<50;++i) {
            if (!manager->checkService(String16("pocket.boot.early-identity"))) {
                puts("DIRECT_REGISTRY_DEATH_PASS service_removed=1");return 0;
            }
            usleep(100000);
        }
        return 7;
    }
    if (argc==2 && !strcmp(argv[1],"identity-client")) {
        sp<IBinder> service=manager->checkService(String16("pocket.boot.early-identity"));
        if (!service) return 4;
        Parcel request,reply;request.writeInterfaceToken(String16("dev.kreijstal.pocket.EarlyIdentity"));
        status_t status=service->transact(IBinder::FIRST_CALL_TRANSACTION,request,&reply);
        bool allowed=getuid()==1000;
        if (allowed ? (status!=NO_ERROR || reply.readInt32()!=1000) : status!=PERMISSION_DENIED) return 5;
        printf("DIRECT_EARLY_IDENTITY_PASS guest_uid=%d permission=%s\n",getuid(),allowed?"allowed":"denied");return 0;
    }
    if (argc!=1) return 6;
    const char* names[]={"power","display","installd"};
    for (const char* name:names) {
        sp<IBinder> service;
        for (int i=0;i<100 && !service;++i) { service=manager->checkService(String16(name)); if (!service) usleep(100000); }
        if (!service || service->pingBinder()!=NO_ERROR) { fprintf(stderr,"DIRECT_BOOT_SERVICE_MISSING %s\n",name);return 2; }
        printf("DIRECT_BOOT_SERVICE name=%s ping=passed\n",name);
    }
    puts("DIRECT_BOOT_SERVICES_PASS power=1 display=1 installd=1");
    return 0;
}
