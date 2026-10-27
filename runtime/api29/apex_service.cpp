// SPDX-License-Identifier: GPL-3.0-only
// Read-only Android 10 APEX inventory for the app-private flattened image.
#include <binder/Binder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <binder/ProcessState.h>
#include <binder/Status.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include "boot_storage_protocol.h"
using namespace android;
struct Module {const char* name;const char* path;int64_t version;const char* versionName;const char* manifest;};
#include "apex_modules.h"

static bool valid(const Module& module) {
    struct stat st;
    if(stat(module.path,&st) || !S_ISDIR(st.st_mode))return false;
    std::string name=std::string(module.path)+"/apex_manifest.json";
    FILE* file=fopen(name.c_str(),"rb");if(!file)return false;
    std::string contents;char buffer[1024];size_t count;
    while((count=fread(buffer,1,sizeof(buffer),file)) && contents.size()<=65536)contents.append(buffer,count);
    bool ok=!ferror(file) && contents==module.manifest;fclose(file);return ok;
}
static void writeModule(Parcel* reply,const Module& module) {
    reply->writeInt32(1); // Typed-object presence.
    size_t start=reply->dataPosition();reply->writeInt32(0);
    reply->writeUtf8AsUtf16(module.name);reply->writeUtf8AsUtf16(module.path);
    reply->writeInt64(module.version);reply->writeUtf8AsUtf16(module.versionName);
    reply->writeInt32(1);reply->writeInt32(1); // Factory and active.
    size_t end=reply->dataPosition();reply->setDataPosition(start);
    reply->writeInt32(end-start);reply->setDataPosition(end);
}
class Apex final : public BBinder {
    status_t onTransact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags) override {
        if(code<IApexService_submitStagedSession || code>IApexService_resumeRollbackIfNeeded)
            return BBinder::onTransact(code,data,reply,flags);
        auto uid=IPCThreadState::self()->getCallingUid();if(uid!=0 && uid!=1000)return PERMISSION_DENIED;
        if(!reply || !data.enforceInterface(String16("android.apex.IApexService")))return BAD_VALUE;
        if(code==IApexService_getAllPackages || code==IApexService_getActivePackages){
            for(const auto& module:modules)if(!valid(module))
                return binder::Status::fromServiceSpecificError(5,"Flattened APEX inventory differs from the installed image").writeToParcel(reply);
            reply->writeInt32(0);reply->writeInt32(sizeof(modules)/sizeof(modules[0]));
            for(const auto& module:modules)writeModule(reply,module);
            puts("API29_APEX_INVENTORY modules=6 flattened=1 updates_supported=0");return NO_ERROR;
        }
        if(code==IApexService_getActivePackage){
            String16 name=data.readString16();
            for(const auto& module:modules)if(name==String16(module.name)){
                if(!valid(module))return BAD_VALUE;
                reply->writeInt32(0);writeModule(reply,module);return NO_ERROR;
            }
            reply->writeInt32(0);reply->writeInt32(0);return NO_ERROR;
        }
        if(code==IApexService_getSessions){reply->writeInt32(0);reply->writeInt32(0);return NO_ERROR;}
        if(code==IApexService_getStagedSessionInfo){
            int32_t session=data.readInt32();reply->writeInt32(0);reply->writeInt32(1);
            reply->writeInt32(44);reply->writeInt32(session);reply->writeInt32(1);
            for(int n=0;n<8;n++)reply->writeInt32(0);
            return NO_ERROR;
        }
        return binder::Status::fromExceptionCode(binder::Status::EX_UNSUPPORTED_OPERATION,
            "Flattened APEX modules are replaced with the Pocket image; staged APEX updates are unavailable").writeToParcel(reply);
    }
};
static int check(const sp<IServiceManager>& manager) {
    auto service=manager->getService(String16("apexservice"));if(!service)return 2;
    for(uint32_t code:{IApexService_getActivePackages,IApexService_getAllPackages}){
        Parcel data,reply;data.writeInterfaceToken(String16("android.apex.IApexService"));
        if(service->transact(code,data,&reply)!=NO_ERROR || reply.readInt32()!=0 || reply.readInt32()!=6)return 3;
        for(const auto& module:modules){
            if(reply.readInt32()!=1)return 4;
            size_t start=reply.dataPosition();int size=reply.readInt32();
            if(size<4 || reply.readString16()!=String16(module.name) || reply.readString16()!=String16(module.path) ||
                reply.readInt64()!=module.version || reply.readString16()!=String16(module.versionName) ||
                reply.readInt32()!=1 || reply.readInt32()!=1 || reply.dataPosition()!=start+size || !valid(module))return 5;
        }
        if(reply.dataAvail())return 6;
    }
    Parcel data,reply;data.writeInterfaceToken(String16("android.apex.IApexService"));
    data.writeUtf8AsUtf16("/data/rejected.apex");binder::Status status;
    if(service->transact(IApexService_stagePackage,data,&reply)!=NO_ERROR || status.readFromParcel(reply)!=NO_ERROR ||
        status.exceptionCode()!=binder::Status::EX_UNSUPPORTED_OPERATION)return 7;
    puts("API29_APEX_PASS active=6 factory=6 manifests_verified=1 staged_updates=rejected");return 0;
}
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);if(getuid()!=0)return 64;
    ProcessState::self()->startThreadPool();auto manager=defaultServiceManager();if(!manager)return 1;
    if(argc==2 && !strcmp(argv[1],"--check"))return check(manager);
    if(argc!=1)return 64;
    for(const auto& module:modules)if(!valid(module))return 2;
    if(manager->addService(String16("apexservice"),new Apex)!=NO_ERROR)return 3;
    puts("API29_APEX_READY modules=6 flattened=1");IPCThreadState::self()->joinThreadPool();return 0;
}
