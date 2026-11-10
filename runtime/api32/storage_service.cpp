#include "service_output.h"
// SPDX-License-Identifier: GPL-3.0-only
// Typed app-private storage, with optional userspace CE encryption.
#include "VoldUnsupported32.h"
#include "storage_files.h"
#include "ce_storage.h"
#include "external_volume.h"
#include "external_app_dirs.h"
#include <android/os/BnStoraged.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <sys/statvfs.h>
#include <sys/system_properties.h>
#include <sys/syscall.h>
#include <atomic>
#include <map>
#include <mutex>
#include <stdio.h>
#include <string.h>

using namespace android;
using namespace android::os;
using binder::Status;
namespace {
bool systemRole() {uid_t uid=IPCThreadState::self()->getCallingUid();return uid==0 || uid==1000;}
Status argument(const char* why) {return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT,why);}
Status failure(const char* why) {return Status::fromServiceSpecificError(errno?errno:EIO,why);}
Status ceResult(int code) {return code<0?Status::fromServiceSpecificError(-code,"App-private CE operation failed"):Status::ok();}
Status singleUser(int user,int serial=0) {
    return user==0 && serial==0?Status::ok():argument("This profile supports persistent user 0 with serial 0");
}
class Vold final : public VoldUnsupported32 {
    std::mutex mutex;
    std::mutex cryptMutex;
    std::mutex volumeMutex;
    sp<IVoldListener> listener;
    sp<IBinder::DeathRecipient> death;
    bool started=false,secureKeyguard=false;
    bool volumeAnnounced=false;
    std::unique_ptr<pocket::fuse32::ExternalVolume> external;
    std::map<std::string,int32_t> appIds;
    class ListenerDeath final : public IBinder::DeathRecipient {
        wp<Vold> owner;
        void binderDied(const wp<IBinder>&) override {
            auto service=owner.promote();if(!service)return;
            std::lock_guard<std::mutex> lock(service->mutex);
            if(service->death.get()==this){service->listener.clear();service->death.clear();}
        }
    public: explicit ListenerDeath(Vold* service):owner(service){}
    };
    bool externalSelected() {
        struct stat state{};
        return !lstat("/system/etc/pocket-external-volume32",&state) && S_ISREG(state.st_mode);
    }
    sp<IVoldListener> volumeListener() {std::lock_guard<std::mutex> lock(mutex);return listener;}
    Status externalResult(int result) {return result<0?Status::fromServiceSpecificError(-result,"Emulated storage session failed"):Status::ok();}
    int stopExternal(const sp<IVoldListener>& target,bool destroy=false) {
        int result=0;
        if(external && external->mounted() && target)target->onVolumeStateChanged("emulated;0",IVold::VOLUME_STATE_EJECTING);
        if(external){result=external->stop();if(external->clean())external.reset();}
        if(result<0)return result;
        if(volumeAnnounced && target) {
            auto state=target->onVolumeStateChanged("emulated;0",IVold::VOLUME_STATE_UNMOUNTED);
            if(!state.isOk() && !result)result=-EPIPE;
            if(destroy){state=target->onVolumeDestroyed("emulated;0");if(!state.isOk() && !result)result=-EPIPE;}
        }
        if(destroy)volumeAnnounced=false;
        return result;
    }
    int configureExternal() {
        using namespace pocket::fuse32;
        if(external && !external->clean())return -EBUSY;
        ExternalVolume::Options options;const char* pid=getenv("POCKET_API32_HOST_PID"),*start=getenv("POCKET_API32_HOST_START");char* end=nullptr;
        if(!pid || !start || *pid<'1' || *pid>'9' || *start<'1' || *start>'9')return -EINVAL;
        errno=0;long host=strtol(pid,&end,10);if(errno || !end || *end || host<=0 || host>INT32_MAX)return -EINVAL;options.hostPid=host;
        errno=0;options.hostStart=strtoull(start,&end,10);if(errno || !end || *end || !options.hostStart)return -EINVAL;
        int result=kernelUid(&options.hostUid);if(result<0)return result;
        external=std::make_unique<ExternalVolume>(options,[options](unsigned operation,uint64_t epoch){return volumeAdapter(options,operation,epoch);});return 0;
    }
    Status prepare(int flags) {
        if(flags<0 || (flags&~3))return argument("Invalid storage flags");
        std::lock_guard<std::mutex> lock(mutex);
        if(ceEnabled32() && (flags&2)) {
            int state=ceControl32(0);if(state<0)return ceResult(state);
            if(state!=2)return ceResult(-EACCES);
        }
        if(!prepareStorageUser032(flags))return failure("Cannot prepare app-private user storage");
        fprintf(stderr,"API32_VOLD_USER_STORAGE_READY user=0 flags=%d encryption=%s kernel_mounts=none\n",flags,ceEnabled32()?"app_private_ce":"none");
        return Status::ok();
    }
    Status appDirectories(const std::vector<std::string>& paths,int32_t uid,pocket::storage32::AppDirectories::Operation operation) {
        using pocket::storage32::AppDirectories;
        if(paths.empty() || paths.size()>128)return argument("Invalid app directory count");
        std::vector<AppDirectories::Plan> plans;plans.reserve(paths.size());
        for(const auto& path:paths){AppDirectories::Plan plan;if(AppDirectories::parse(path,uid,&plan)<0)return argument("Invalid app directory path or UID");plans.push_back(std::move(plan));}
        std::lock_guard<std::mutex> volume(volumeMutex);
        if(!externalSelected())return storageUnsupported32("app_directory_without_external_volume");
        if(!external || !external->mounted())return externalResult(-ENODEV);
        {std::lock_guard<std::mutex> lock(mutex);for(const auto& plan:plans){auto known=appIds.find(plan.package);if(known!=appIds.end() && known->second!=uid)return Status::fromExceptionCode(Status::EX_SECURITY,"Package/app UID mismatch");}}
        std::lock_guard<std::mutex> crypt(cryptMutex);
        if(ceEnabled32()){int state=ceControl32(0);if(state!=2)return ceResult(state<0?state:-EACCES);}
        int root=open("/data/media/0",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(root<0)return failure("Cannot open user media storage");
        int result=0;for(const auto& plan:plans){result=AppDirectories::prepare(root,plan,operation);if(result<0)break;}close(root);
        fprintf(stderr,"API32_VOLD_APP_DIRECTORIES result=%d uid=%d paths=%zu kernel_mounts=none host_quota_acl=unsupported\n",result,uid,paths.size());return externalResult(result);
    }
    status_t onTransact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags) override {
        if(code>=FIRST_CALL_TRANSACTION && code<=LAST_CALL_TRANSACTION && !systemRole())
            return reply?Status::fromExceptionCode(Status::EX_SECURITY,"Storage requires a guest system role").writeToParcel(reply):PERMISSION_DENIED;
        return BnVold::onTransact(code,data,reply,flags);
    }
public:
    Status setupAppDir(const std::string& path,int32_t uid) override {return appDirectories({path},uid,pocket::storage32::AppDirectories::Operation::Setup);}
    Status fixupAppDir(const std::string& path,int32_t uid) override {return appDirectories({path},uid,pocket::storage32::AppDirectories::Operation::FixExisting);}
    Status ensureAppDirsCreated(const std::vector<std::string>& paths,int32_t uid) override {return appDirectories(paths,uid,pocket::storage32::AppDirectories::Operation::EnsureExists);}
    Status setListener(const sp<IVoldListener>& value) override {
        if(!value)return argument("A volume listener is required");
        auto binder=IInterface::asBinder(value);
        if(!binder || binder->pingBinder()!=OK)return argument("Listener is not live");
        sp<IBinder::DeathRecipient> next=new ListenerDeath(this);
        std::lock_guard<std::mutex> lock(mutex);
        auto linked=binder->linkToDeath(next);
        if(linked!=OK && linked!=INVALID_OPERATION)return Status::fromStatusT(linked);
        if(listener)IInterface::asBinder(listener)->unlinkToDeath(death);
        listener=value;death=next;return Status::ok();
    }
    Status monitor() override {
        std::lock_guard<std::mutex> lock(mutex);struct statvfs state;
        return !statvfs("/data",&state) && state.f_bsize?Status::ok():failure("Private storage unavailable");
    }
    Status reset() override {
        std::lock_guard<std::mutex> operation(volumeMutex);int result=stopExternal(volumeListener(),true);
        if(result<0)return externalResult(result);
        {std::lock_guard<std::mutex> lock(mutex);started=false;}
        // Framework owns its built-in private /data volume. User0 start may
        // announce an emulated volume only for an explicitly selected backend.
        fprintf(stderr,"API32_VOLD_RESET physical_disks=0 external_volumes=0 external_backend=%s\n",externalSelected()?"selected":"disabled");return externalResult(result);
    }
    Status shutdown() override {std::lock_guard<std::mutex> operation(volumeMutex);return externalResult(stopExternal(volumeListener()));}
    Status abortFuse() override {std::lock_guard<std::mutex> operation(volumeMutex);return externalResult(stopExternal(volumeListener()));}
    Status mount(const std::string& id,int32_t flags,int32_t user,const sp<IVoldMountCallback>& callback) override {
        if(!externalSelected())return storageUnsupported32("mount");
        if(id!="emulated;0" || user!=0 || flags&~3 || !callback)return argument("Invalid emulated-volume mount");
        std::lock_guard<std::mutex> operation(volumeMutex);auto target=volumeListener();
        if(!target || !volumeAnnounced)return externalResult(-ENODEV);
        if(external && external->mounted())return externalResult(-EBUSY);
        int result=configureExternal();if(result<0)return externalResult(result);
        auto state=target->onVolumeStateChanged(id,IVold::VOLUME_STATE_CHECKING);if(!state.isOk()){external.reset();return externalResult(-EPIPE);}
        pocket::fuse32::Identity initializer{0,0,static_cast<uint32_t>(syscall(__NR_gettid))};
        result=external->mount([&](int daemon){bool accepted=false;int copy=fcntl(daemon,F_DUPFD_CLOEXEC,0);if(copy<0)return -errno;
            auto status=callback->onVolumeChecking(base::unique_fd(copy),"/storage/emulated","/data/media",&accepted);return status.isOk() && accepted?0:-EIO;},initializer);
        if(!result && !target->onVolumePathChanged(id,"/storage/emulated").isOk())result=-EPIPE;
        if(!result && !target->onVolumeInternalPathChanged(id,"/data/media").isOk())result=-EPIPE;
        if(!result && !target->onVolumeMetadataChanged(id,"emulated","","").isOk())result=-EPIPE;
        if(!result && !target->onVolumeStateChanged(id,IVold::VOLUME_STATE_MOUNTED).isOk())result=-EPIPE;
        if(result<0){external->stop();if(external->clean())external.reset();target->onVolumeStateChanged(id,IVold::VOLUME_STATE_UNMOUNTABLE);}
        fprintf(stderr,"API32_EXTERNAL_VOLUME_MOUNT result=%d user=0 kernel_mounts=none\n",result);return externalResult(result);
    }
    Status unmount(const std::string& id) override {
        if(!externalSelected())return storageUnsupported32("unmount");
        if(id!="emulated;0")return argument("Unknown emulated volume");
        std::lock_guard<std::mutex> operation(volumeMutex);return externalResult(stopExternal(volumeListener()));
    }
    Status onUserAdded(int32_t user,int32_t serial) override {return singleUser(user,serial);}
    Status onUserStarted(int32_t user) override {
        auto status=singleUser(user);if(!status.isOk())return status;
        std::lock_guard<std::mutex> operation(volumeMutex);
        {std::lock_guard<std::mutex> lock(mutex);started=true;}
        if(externalSelected() && !volumeAnnounced) {
            auto target=volumeListener();if(!target)return externalResult(-ENODEV);
            int result=configureExternal();if(result<0)return externalResult(result);
            status=target->onVolumeCreated("emulated;0",IVold::VOLUME_TYPE_EMULATED,"","",0);
            if(status.isOk())volumeAnnounced=true;else external.reset();return status;
        }
        return Status::ok();
    }
    Status onUserStopped(int32_t user) override {
        auto status=singleUser(user);if(!status.isOk())return status;
        std::lock_guard<std::mutex> operation(volumeMutex);int result=stopExternal(volumeListener(),true);
        if(result<0)return externalResult(result);
        {std::lock_guard<std::mutex> lock(mutex);started=false;}return externalResult(result);
    }
    Status onSecureKeyguardStateChanged(bool value) override {
        std::lock_guard<std::mutex> lock(mutex);secureKeyguard=value;return Status::ok();
    }
    Status addAppIds(const std::vector<std::string>& packages,const std::vector<int32_t>& ids) override {
        if(packages.size()!=ids.size() || packages.size()>32768)return argument("Invalid app ID mapping");
        for(size_t i=0;i<packages.size();i++) {
            if(packages[i].empty() || packages[i].size()>255 || ids[i]<0 || ids[i]>=100000)
                return argument("Invalid package/app ID");
            for(char c:packages[i])if(!((c>='a' && c<='z') || (c>='A' && c<='Z') ||
                (c>='0' && c<='9') || c=='.' || c=='_'))return argument("Invalid package name");
        }
        std::lock_guard<std::mutex> lock(mutex);
        auto next=appIds;for(size_t i=0;i<packages.size();i++)next[packages[i]]=ids[i];
        if(next.size()>32768)return argument("App ID map is full");
        appIds.swap(next);return Status::ok();
    }
    Status initUser0() override {return prepare(1);}
    Status prepareUserStorage(const std::optional<std::string>& uuid,int32_t user,int32_t serial,int32_t flags) override {
        auto status=singleUser(user,serial);if(!status.isOk())return status;
        if(uuid && !uuid->empty())return storageUnsupported32("prepare_adopted_volume");
        return prepare(flags);
    }
    Status unlockUserKey(int32_t user,int32_t serial,const std::string& token,const std::string& secret) override {
        std::lock_guard<std::mutex> crypt(cryptMutex);
        auto status=singleUser(user,serial);if(!status.isOk())return status;
        if(!token.empty() && token!="!")return argument("Auth tokens are not used by this volume service");
        if(ceEnabled32()) {
            CeSecret32 decoded;if(!decoded.decode(secret))return argument("Invalid hex-encoded CE secret");
            auto result=ceResult(ceControl32(1,decoded.bytes));if(!result.isOk())return result;
        }else if(!secret.empty() && secret!="!") {
            int armed=ceControl32(10);
            if(armed==1) {
                CeSecret32 decoded;if(!decoded.decode(secret))return argument("Invalid hex-encoded CE secret");
                auto result=ceResult(ceControl32(4,decoded.bytes));if(!result.isOk())return result;
            }else if(armed<0 && armed!=-ENOENT && armed!=-ECONNREFUSED)return ceResult(armed);
        }
        // Stock non-FBE vold prepares access without installing an fscrypt key.
        return prepare(3);
    }
    Status lockUserKey(int32_t user) override {
        std::lock_guard<std::mutex> crypt(cryptMutex);
        auto status=singleUser(user);if(!status.isOk())return status;
        return ceEnabled32()?ceResult(ceControl32(2)):Status::ok();
    }
    Status createUserKey(int32_t user,int32_t serial,bool ephemeral) override {
        std::lock_guard<std::mutex> crypt(cryptMutex);
        auto status=singleUser(user,serial);if(!status.isOk())return status;
        if(ephemeral)return storageUnsupported32("ephemeral_user_key");
        if(!ceEnabled32())return Status::ok();
        int state=ceControl32(0);return ceResult(state<0?state:state==1 || state==2?0:-EIO);
    }
    Status addUserKeyAuth(int32_t user,int32_t serial,const std::string& token,const std::string& secret) override {
        std::lock_guard<std::mutex> crypt(cryptMutex);
        auto status=singleUser(user,serial);if(!status.isOk())return status;
        if(!token.empty() && token!="!")return argument("Auth tokens are not used by this volume service");
        if(!ceEnabled32())return Status::ok();
        CeSecret32 decoded;if(!decoded.decode(secret))return argument("Invalid hex-encoded CE secret");
        return ceResult(ceControl32(3,decoded.bytes));
    }
    Status clearUserKeyAuth(int32_t user,int32_t serial,const std::string& token,const std::string& secret) override {
        std::lock_guard<std::mutex> crypt(cryptMutex);
        auto status=singleUser(user,serial);if(!status.isOk())return status;
        if(!token.empty() && token!="!")return argument("Auth tokens are not used by this volume service");
        if(!ceEnabled32())return Status::ok();
        CeSecret32 decoded;if(!decoded.decode(secret))return argument("Invalid hex-encoded CE secret");
        auto unlocked=ceResult(ceControl32(1,decoded.bytes));if(!unlocked.isOk())return unlocked;
        return ceResult(ceControl32(3));
    }
    Status fixateNewestUserKeyAuth(int32_t user) override {
        std::lock_guard<std::mutex> crypt(cryptMutex);
        auto status=singleUser(user);if(!status.isOk())return status;
        return ceEnabled32()?ceResult(ceControl32(9)):Status::ok();
    }
    Status getUnlockedUsers(std::vector<int32_t>* result) override {
        std::lock_guard<std::mutex> crypt(cryptMutex);
        result->clear();if(!ceEnabled32())return Status::ok();
        int state=ceControl32(0);if(state<0)return ceResult(state);
        if(state==2)result->push_back(0);return Status::ok();
    }
    Status mountDefaultEncrypted() override {return monitor();} // Existing plaintext /data, no mount.
    Status fdeComplete(int32_t* result) override {*result=ceEnabled32()?IVold::ENCRYPTION_STATE_OK:IVold::ENCRYPTION_STATE_NONE;return Status::ok();}
    Status isConvertibleToFbe(bool* result) override {*result=false;return Status::ok();}
    Status supportsCheckpoint(bool* result) override {*result=false;return Status::ok();}
    Status supportsBlockCheckpoint(bool* result) override {*result=false;return Status::ok();}
    Status supportsFileCheckpoint(bool* result) override {*result=false;return Status::ok();}
    Status needsCheckpoint(bool* result) override {*result=false;return Status::ok();}
    Status needsRollback(bool* result) override {*result=false;return Status::ok();}
    Status isCheckpointing(bool* result) override {*result=false;return Status::ok();}
    Status incFsEnabled(bool* result) override {*result=false;return Status::ok();}
    Status commitChanges() override {return Status::ok();} // No checkpoint is active.
    Status prepareCheckpoint() override {return Status::ok();}
    Status markBootAttempt() override {return Status::ok();}
    Status resetCheckpoint() override {return Status::ok();}
    Status maintenance(const sp<IVoldTaskListener>& target) {
        if(!target)return argument("A maintenance listener is required");
        auto result=target->onFinished(-EOPNOTSUPP,PersistableBundle());
        return result.isOk()?Status::ok():Status::fromServiceSpecificError(EPIPE,"Maintenance callback unavailable");
    }
    Status fstrim(int32_t flags,const sp<IVoldTaskListener>& target) override {
        return flags<0 || (flags&~IVold::FSTRIM_FLAG_DEEP_TRIM)?argument("Invalid trim flags"):maintenance(target);
    }
    Status runIdleMaint(const sp<IVoldTaskListener>& target) override {return maintenance(target);}
    Status abortIdleMaint(const sp<IVoldTaskListener>& target) override {return maintenance(target);}
};
class Storaged final : public BnStoraged {
    status_t onTransact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags) override {
        if(code>=FIRST_CALL_TRANSACTION && code<=LAST_CALL_TRANSACTION && !systemRole())
            return reply?Status::fromExceptionCode(Status::EX_SECURITY,"Storage stats require a system role").writeToParcel(reply):PERMISSION_DENIED;
        return BnStoraged::onTransact(code,data,reply,flags);
    }
public:
    Status onUserStarted(int32_t user) override {return singleUser(user);}
    Status onUserStopped(int32_t user) override {return singleUser(user);}
    Status getRecentPerf(int32_t* result) override {*result=0;return Status::ok();} // Stock no-samples result.
};
}
int main() {
    pocketServiceOutput32();char sdk[PROP_VALUE_MAX],crypto[PROP_VALUE_MAX];
    if(syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL || getuid()!=0 ||
        __system_property_get("ro.build.version.sdk",sdk)<=0 || strcmp(sdk,"32"))return 64;
    __system_property_get("ro.crypto.state",crypto);
    if(ceEnabled32()) {
        int state=ceControl32(0);
        if(syscall(__NR_getpid,0x504c2001UL,5UL)!=0x504c3202UL || (state!=1 && state!=2)) {
            fputs("API32_STORAGE_CE_BACKEND_UNAVAILABLE\n",stderr);return 65;
        }
    }else if(!strcmp(crypto,"encrypted")) {
        fputs("API32_STORAGE_ENCRYPTED_PROPERTY_WITHOUT_BACKEND\n",stderr);return 65;
    }
    ProcessState::self()->startThreadPool();auto manager=defaultServiceManager();if(!manager)return 1;
    sp<Vold> vold=new Vold;
    if(!vold->initUser0().isOk() || manager->addService(String16("vold"),vold)!=OK ||
        manager->addService(String16("storaged"),new Storaged)!=OK)return 2;
    printf("API32_STORAGE_READY typed_platform=12L internal_data=app_private encryption=%s external_fuse=selected_per_image checkpoint=unsupported\n",ceEnabled32()?"app_private_ce":"none");
    IPCThreadState::self()->joinThreadPool();return 0;
}
