#include "service_output.h"
// SPDX-License-Identifier: GPL-3.0-only
// Typed 12L inventory for the verified, read-only flattened APEX profile.
#include <android/apex/BnApexService.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

using namespace android;
using namespace android::apex;
using binder::Status;
struct Module {
    const char *name,*path,*preinstalled,*versionName;
    int64_t version;
    const unsigned char* manifest;
    size_t bytes;
};
#include "apex_modules32.h"

namespace {
int directoryAt(int parent,const char* name,mode_t mode,uid_t uid,gid_t gid) {
    if(mkdirat(parent,name,mode) && errno!=EEXIST)return -1;
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return -1;
    struct stat st;
    if(fchown(fd,uid,gid) || fchmod(fd,mode) || fstat(fd,&st) ||
       !S_ISDIR(st.st_mode) || st.st_uid!=uid || st.st_gid!=gid || (st.st_mode&07777)!=mode) {
        close(fd);return -1;
    }
    return fd;
}
bool moduleName(const char* name) {
    if(!name || !*name || *name=='.')return false;
    for(const char* p=name;*p;p++)if(!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') ||
        (*p>='0' && *p<='9') || *p=='.' || *p=='_' || *p=='-'))return false;
    return true;
}
bool prepareDeviceData() {
    // Same shared-data ownership/modes as 12L init's create_apex_data_dirs.
    // Only modules in the digest-verified image inventory are prepared.
    // User CE/DE preparation belongs to the storage service, not this step.
    int data=open("/data",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(data<0)return false;
    int misc=directoryAt(data,"misc",01771,1000,9998);close(data);if(misc<0)return false;
    int apex=directoryAt(misc,"apexdata",0711,0,0);close(misc);if(apex<0)return false;
    bool ready=true;
    for(const auto& module:modules) {
        int fd=moduleName(module.name)?directoryAt(apex,module.name,0771,0,1000):-1;
        if(fd<0){ready=false;break;}close(fd);
    }
    close(apex);
    if(ready)printf("API32_APEX_DATA_READY modules=%zu shared_device_data=1 owner=0 group=1000 mode=0771\n",
        sizeof(modules)/sizeof(modules[0]));
    return ready;
}
int checkData() {
    for(const auto& module:modules) {
        struct stat st;std::string path="/data/misc/apexdata/"+std::string(module.name);
        if(lstat(path.c_str(),&st) || !S_ISDIR(st.st_mode) || st.st_uid!=0 ||
           st.st_gid!=1000 || (st.st_mode&07777)!=0771)return 20;
    }
    uint64_t nonce;if(getrandom(&nonce,sizeof(nonce),0)!=sizeof(nonce))return 21;
    char file[160];snprintf(file,sizeof(file),"/data/misc/apexdata/com.android.wifi/.pocket-data-test-%016llx",
        static_cast<unsigned long long>(nonce));
    constexpr char message[]="api32-module-data\n";
    pid_t child=fork();if(child<0)return 22;
    if(!child) {
        if(setgid(1000) || setuid(1000))_exit(1);
        int fd=open(file,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(fd<0)_exit(2);
        bool wrote=write(fd,message,sizeof(message))==sizeof(message);
        int closed=close(fd);_exit(wrote && !closed?0:3);
    }
    int status;while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 23;
    if(!WIFEXITED(status) || WEXITSTATUS(status))return 24;
    int fd=open(file,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return 25;
    struct stat st;char bytes[sizeof(message)];
    bool readBack=read(fd,bytes,sizeof(bytes))==sizeof(bytes) && !memcmp(bytes,message,sizeof(bytes)) &&
        !fstat(fd,&st) && st.st_uid==1000 && (st.st_mode&07777)==0600;
    close(fd);if(unlink(file) || !readBack)return 26;
    puts("API32_APEX_DATA_PASS image_modules=verified shared_device_data=1 ownership_modes=verified system_write_read=1 cleanup=1");
    return 0;
}
bool valid(const Module& module) {
    struct stat st;
    if(stat(module.path,&st) || !S_ISDIR(st.st_mode))return false;
    FILE* file=fopen((std::string(module.path)+"/apex_manifest.pb").c_str(),"rb");
    if(!file)return false;
    std::vector<unsigned char> bytes(module.bytes);
    bool result=fread(bytes.data(),1,bytes.size(),file)==bytes.size() &&
        fgetc(file)==EOF && !ferror(file) && !memcmp(bytes.data(),module.manifest,bytes.size());
    fclose(file);return result;
}
ApexInfo describe(const Module& module) {
    ApexInfo info;
    info.moduleName=module.name;info.modulePath=module.path;
    info.preinstalledModulePath=module.preinstalled;info.versionName=module.versionName;
    info.versionCode=module.version;info.isFactory=true;info.isActive=true;
    return info;
}
Status unsupported() {
    return Status::fromExceptionCode(Status::EX_UNSUPPORTED_OPERATION,
        "APEX modules are replaced with the Pocket image; mutable APEX operations are unavailable");
}
class Apex final : public BnApexService {
    status_t onTransact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags) override {
        uid_t uid=IPCThreadState::self()->getCallingUid();
        if(code>=FIRST_CALL_TRANSACTION && code<=LAST_CALL_TRANSACTION && uid!=0 && uid!=1000)
            return reply?Status::fromExceptionCode(Status::EX_SECURITY,"APEX inventory requires a guest system role").writeToParcel(reply):PERMISSION_DENIED;
        return BnApexService::onTransact(code,data,reply,flags);
    }
public:
    Status getActivePackages(std::vector<ApexInfo>* result) override {
        result->clear();
        for(const auto& module:modules) {
            if(!valid(module))return Status::fromServiceSpecificError(EIO,"Installed APEX manifest differs from the image");
            result->push_back(describe(module));
        }
        return Status::ok();
    }
    Status getAllPackages(std::vector<ApexInfo>* result) override {return getActivePackages(result);}
    Status getActivePackage(const std::string& name,ApexInfo* result) override {
        *result=ApexInfo();
        for(const auto& module:modules)if(name==module.name) {
            if(!valid(module))return Status::fromServiceSpecificError(EIO,"Installed APEX manifest differs from the image");
            *result=describe(module);break;
        }
        return Status::ok();
    }
    Status getSessions(std::vector<ApexSessionInfo>* result) override {result->clear();return Status::ok();}
    Status getStagedSessionInfo(int32_t,ApexSessionInfo* result) override {
        *result=ApexSessionInfo();result->sessionId=-1;result->isUnknown=true;return Status::ok();
    }
    Status markBootCompleted() override {
        return __system_property_set("pocket.apex.boot_completed","1") ?
            Status::fromServiceSpecificError(EIO,"Cannot record guest APEX boot notification") : Status::ok();
    }
#define UNAVAILABLE(name,...) Status name(__VA_ARGS__) override {return unsupported();}
    UNAVAILABLE(submitStagedSession,const ApexSessionParams&,ApexInfoList*)
    UNAVAILABLE(markStagedSessionReady,int32_t)
    UNAVAILABLE(markStagedSessionSuccessful,int32_t)
    UNAVAILABLE(abortStagedSession,int32_t)
    UNAVAILABLE(revertActiveSessions)
    UNAVAILABLE(snapshotCeData,int32_t,int32_t,const std::string&)
    UNAVAILABLE(restoreCeData,int32_t,int32_t,const std::string&)
    UNAVAILABLE(destroyDeSnapshots,int32_t)
    UNAVAILABLE(destroyCeSnapshots,int32_t,int32_t)
    UNAVAILABLE(destroyCeSnapshotsNotSpecified,int32_t,const std::vector<int32_t>&)
    UNAVAILABLE(unstagePackages,const std::vector<std::string>&)
    UNAVAILABLE(activatePackage,const std::string&)
    UNAVAILABLE(deactivatePackage,const std::string&)
    UNAVAILABLE(preinstallPackages,const std::vector<std::string>&)
    UNAVAILABLE(postinstallPackages,const std::vector<std::string>&)
    UNAVAILABLE(stagePackages,const std::vector<std::string>&)
    UNAVAILABLE(resumeRevertIfNeeded)
    UNAVAILABLE(remountPackages)
    UNAVAILABLE(recollectPreinstalledData,const std::vector<std::string>&)
    UNAVAILABLE(recollectDataApex,const std::string&,const std::string&)
    UNAVAILABLE(calculateSizeForCompressedApex,const CompressedApexInfoList&,int64_t*)
    UNAVAILABLE(reserveSpaceForCompressedApex,const CompressedApexInfoList&)
    UNAVAILABLE(installAndActivatePackage,const std::string&,ApexInfo*)
#undef UNAVAILABLE
};

int check(const sp<IApexService>& service) {
    std::vector<ApexInfo> active,all;
    if(!service->getActivePackages(&active).isOk() || !service->getAllPackages(&all).isOk() ||
        active.size()!=sizeof(modules)/sizeof(modules[0]) || active!=all)return 3;
    for(size_t i=0;i<active.size();i++) {
        ApexInfo single;
        if(active[i]!=describe(modules[i]) || !valid(modules[i]) ||
            !service->getActivePackage(modules[i].name,&single).isOk() || single!=active[i])return 4;
    }
    ApexInfo absent;std::vector<ApexSessionInfo> sessions;ApexSessionInfo unknown;
    if(!service->getActivePackage("dev.kreijstal.pocket.absent",&absent).isOk() || !absent.moduleName.empty() ||
        !service->getSessions(&sessions).isOk() || !sessions.empty() ||
        !service->getStagedSessionInfo(32,&unknown).isOk() || !unknown.isUnknown || unknown.sessionId!=-1)return 5;
    if(service->installAndActivatePackage("/data/rejected.apex",&absent).exceptionCode()!=Status::EX_UNSUPPORTED_OPERATION ||
        service->stagePackages({"/data/rejected.apex"}).exceptionCode()!=Status::EX_UNSUPPORTED_OPERATION)return 6;
    if(!service->markBootCompleted().isOk())return 7;
    printf("API32_APEX_PASS modules=%zu factory=1 active=1 manifests_verified=1 typed_parcelables=1 updates=rejected\n",active.size());
    return 0;
}
}
int main(int argc,char** argv) {
    pocketServiceOutput32();
    if(argc!=2 || getuid()!=0)return 64;
    if(!strcmp(argv[1],"check-data"))return checkData();
    if(!strcmp(argv[1],"denied") && (setgid(39999) || setuid(39999)))return 65;
    ProcessState::self()->startThreadPool();
    auto manager=defaultServiceManager();if(!manager)return 1;
    if(!strcmp(argv[1],"serve")) {
        for(const auto& module:modules)if(!valid(module) || !moduleName(module.name))return 2;
        if(!prepareDeviceData())return 9;
        if(manager->addService(String16("apexservice"),new Apex)!=NO_ERROR)return 2;
        puts("API32_APEX_READY flattened=1 updates_supported=0");
        IPCThreadState::self()->joinThreadPool();return 0;
    }
    auto service=interface_cast<IApexService>(manager->checkService(String16("apexservice")));
    if(!service)return 2;
    if(!strcmp(argv[1],"check"))return check(service);
    if(!strcmp(argv[1],"denied")) {
        std::vector<ApexInfo> result;
        if(service->getActivePackages(&result).exceptionCode()!=Status::EX_SECURITY)return 8;
        puts("API32_APEX_ROLE_DENIED_PASS uid=39999");return 0;
    }
    return 64;
}
