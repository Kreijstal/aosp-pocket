// SPDX-License-Identifier: GPL-3.0-only
// App-private storage service profile, using the pinned Android 9 AIDL wire ABI.
// No host mounts, device formatting, filesystem trim or encryption operations.
#include <binder/Binder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <binder/PersistableBundle.h>
#include <binder/ProcessState.h>
#include <binder/Status.h>
#include <utils/Mutex.h>
#include <utils/String8.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <errno.h>
#include <atomic>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "boot_storage_protocol.h"
using namespace android;
namespace {
status_t unsupported(Parcel* reply) {
    return binder::Status::fromExceptionCode(binder::Status::EX_UNSUPPORTED_OPERATION,
        "Operation unavailable in app-private storage profile").writeToParcel(reply);
}
bool systemCaller() {
    auto uid=IPCThreadState::self()->getCallingUid();return uid==0 || uid==1000;
}
bool userDirectoryReady() {
    struct stat st;
    for(const char* path:{"/data/user/0","/data/user_de/0","/data/system_ce/0","/data/system_de/0","/data/media/0"})
        if(stat(path,&st) || !S_ISDIR(st.st_mode)) return false;
    return true;
}
class Storaged final : public BBinder {
    status_t onTransact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags) override {
        if(code<1 || code>3) return BBinder::onTransact(code,data,reply,flags);
        if(!systemCaller()) return PERMISSION_DENIED;
        if(!reply || !data.enforceInterface(String16("android.os.IStoraged"))) return BAD_VALUE;
        if(code==IStoraged_getRecentPerf) {reply->writeInt32(0);reply->writeInt32(0);return NO_ERROR;}
        if(data.readInt32()!=0) return unsupported(reply);
        reply->writeInt32(0);return NO_ERROR;
    }
};
class Vold final : public BBinder {
    Mutex mutex;
    sp<IBinder> listener;
    sp<IBinder> currentListener() {Mutex::Autolock lock(mutex);return listener;}
    template<typename Write> void notify(uint32_t code,Write write) {
        auto target=currentListener();if(!target)return;
        Parcel request;request.writeInterfaceToken(String16("android.os.IVoldListener"));write(request);
        auto status=target->transact(code,request,nullptr,IBinder::FLAG_ONEWAY);
        if(status!=NO_ERROR) fprintf(stderr,"DIRECT_STORAGE_NOTIFICATION_FAILED status=%d\n",status);
    }
    void announce() {
        notify(IVoldListener_onVolumeCreated,[](Parcel& p){p.writeString16(String16("emulated"));p.writeInt32(2);p.writeString16(String16());p.writeString16(String16());});
    }
    void mounted() {
        notify(IVoldListener_onVolumePathChanged,[](Parcel& p){p.writeString16(String16("emulated"));p.writeString16(String16("/storage/emulated"));});
        notify(IVoldListener_onVolumeInternalPathChanged,[](Parcel& p){p.writeString16(String16("emulated"));p.writeString16(String16("/data/media"));});
        notify(IVoldListener_onVolumeStateChanged,[](Parcel& p){p.writeString16(String16("emulated"));p.writeInt32(2);});
        fprintf(stderr,"DIRECT_STORAGE_MOUNTED profile=app_private path=/storage/emulated kernel_mount=none\n");
    }
    void finishUnsupported(const sp<IBinder>& target) {
        if(!target)return;
        Parcel request;request.writeInterfaceToken(String16("android.os.IVoldTaskListener"));
        request.writeInt32(-ENOTSUP);request.writeInt32(1);
        os::PersistableBundle extras;extras.writeToParcel(&request);
        auto status=target->transact(IVoldTaskListener_onFinished,request,nullptr,IBinder::FLAG_ONEWAY);
        fprintf(stderr,"DIRECT_STORAGE_MAINTENANCE result=unsupported callback_status=%d\n",status);
    }
    status_t onTransact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags) override {
        if(code<IVold_setListener || code>IVold_destroyUserStorage) return BBinder::onTransact(code,data,reply,flags);
        if(!systemCaller()) return PERMISSION_DENIED;
        if(!reply || !data.enforceInterface(String16("android.os.IVold"))) return BAD_VALUE;
        switch(code) {
        case IVold_setListener: {
            auto value=data.readStrongBinder();if(!value)return BAD_VALUE;
            {Mutex::Autolock lock(mutex);listener=value;}
            break;
        }
        case IVold_monitor:case IVold_shutdown:break;
        case IVold_reset:announce();break;
        case IVold_onUserAdded:
            if(data.readInt32()!=0) return unsupported(reply);
            data.readInt32();break;
        case IVold_onUserStarted:case IVold_onUserStopped:
            if(data.readInt32()!=0) return unsupported(reply);
            break;
        case IVold_onSecureKeyguardStateChanged:data.readInt32();break;
        case IVold_fdeComplete:reply->writeInt32(0);reply->writeInt32(1);return NO_ERROR;
        case IVold_isConvertibleToFbe:reply->writeInt32(0);reply->writeInt32(0);return NO_ERROR;
        case IVold_initUser0:if(!userDirectoryReady())return BAD_VALUE;break;
        case IVold_unlockUserKey: {
            int user=data.readInt32();data.readInt32();String16 token=data.readString16(),secret=data.readString16();
            // StorageManagerService.encodeBytes uses "!" for absent bytes.
            bool emptyToken=!token.size() || token==String16("!");
            bool emptySecret=!secret.size() || secret==String16("!");
            if(user!=0 || !emptyToken || !emptySecret || !userDirectoryReady()) return unsupported(reply);
            fprintf(stderr,"DIRECT_STORAGE_USER_READY user=0 encryption=none\n");break;
        }
        case IVold_prepareUserStorage: {
            String16 uuid=data.readString16();int user=data.readInt32();data.readInt32();int storage=data.readInt32();
            if(uuid.size() || user!=0 || (storage&~3) || !userDirectoryReady()) return unsupported(reply);
            break;
        }
        case IVold_mount: {
            String16 volume=data.readString16();int mountFlags=data.readInt32(),user=data.readInt32();
            if(volume!=String16("emulated") || (mountFlags&~3) || user!=0) return unsupported(reply);
            mounted();break;
        }
        case IVold_remountUid: {
            data.readInt32();int mode=data.readInt32();if(mode<0 || mode>3)return unsupported(reply);
            break; // Same private paths; no kernel mount or access-mode isolation.
        }
        case IVold_fstrim:data.readInt32();finishUnsupported(data.readStrongBinder());break;
        case IVold_runIdleMaint:case IVold_abortIdleMaint:finishUnsupported(data.readStrongBinder());break;
        default:
            fprintf(stderr,"DIRECT_STORAGE_UNSUPPORTED transaction=%u\n",code);return unsupported(reply);
        }
        reply->writeInt32(0);return NO_ERROR;
    }
};
class TaskFailure final : public BBinder {
    status_t onTransact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags) override {
        if(code!=IVoldTaskListener_onFinished)return BBinder::onTransact(code,data,reply,flags);
        if(!systemCaller() || !data.enforceInterface(String16("android.os.IVoldTaskListener")))return PERMISSION_DENIED;
        int result=data.readInt32();
        if(data.readInt32()!=1)return BAD_VALUE;
        os::PersistableBundle extras;if(extras.readFromParcel(&data)!=NO_ERROR || !extras.empty())return BAD_VALUE;
        outcome=result;return NO_ERROR;
    }
public:
    std::atomic<int> outcome{INT_MIN};
};
int verify(const sp<IServiceManager>& manager,const char* executable) {
    auto service=manager->getService(String16("vold"));if(!service)return 2;
    Parcel request,reply;request.writeInterfaceToken(String16("android.os.IVold"));
    if(service->transact(IVold_fdeComplete,request,&reply)!=NO_ERROR || reply.readInt32()!=0 || reply.readInt32()!=1)return 3;
    Parcel unlock,unlocked;unlock.writeInterfaceToken(String16("android.os.IVold"));
    unlock.writeInt32(0);unlock.writeInt32(0);unlock.writeString16(String16("!"));unlock.writeString16(String16("!"));
    if(service->transact(IVold_unlockUserKey,unlock,&unlocked)!=NO_ERROR || unlocked.readInt32()!=0)return 10;
    ProcessState::self()->startThreadPool();sp<TaskFailure> callback=new TaskFailure;
    Parcel trim,trimReply;trim.writeInterfaceToken(String16("android.os.IVold"));trim.writeInt32(0);trim.writeStrongBinder(callback);
    if(service->transact(IVold_fstrim,trim,&trimReply)!=NO_ERROR || trimReply.readInt32()!=0)return 11;
    for(int n=0;n<500 && callback->outcome==INT_MIN;++n)usleep(10000);
    if(callback->outcome!=-ENOTSUP)return 12;
    puts("DIRECT_STORAGE_CALLBACK_PASS maintenance=unsupported nested_binder_delivery=1 bundle=decoded");
    pid_t child=fork();if(child<0)return 4;
    if(!child) {
        if(setgid(39999) || setuid(39999))_exit(5);
        execl(executable,executable,"verify-denied",nullptr);_exit(6);
    }
    int status;if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status))return 7;
    puts("DIRECT_STORAGE_PROTOCOL_PASS plaintext_profile=1 ordinary_role_denied=1");return 0;
}
}
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);
    auto manager=defaultServiceManager();if(!manager)return 1;
    if(argc==2 && !strcmp(argv[1],"verify"))return verify(manager,argv[0]);
    if(argc==2 && !strcmp(argv[1],"verify-denied")) {
        auto service=manager->getService(String16("vold"));if(!service || getuid()!=39999)return 8;
        Parcel denied,unused;denied.writeInterfaceToken(String16("android.os.IVold"));
        return service->transact(IVold_reset,denied,&unused)==PERMISSION_DENIED?0:9;
    }
    if(argc!=1 || getuid()!=0)return 64;
    ProcessState::self()->startThreadPool();
    if(manager->addService(String16("vold"),new Vold)!=NO_ERROR || manager->addService(String16("storaged"),new Storaged)!=NO_ERROR)return 2;
    if(__system_property_set("pocket.storage.ready","1"))return 3;
    puts("DIRECT_STORAGE_READY profile=app_private host_mounts=unchanged");
    IPCThreadState::self()->joinThreadPool();return 0;
}
