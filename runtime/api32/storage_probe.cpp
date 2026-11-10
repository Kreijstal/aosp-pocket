// SPDX-License-Identifier: GPL-3.0-only
// Independent typed platform client and real private-filesystem checks.
#include <android/os/IVold.h>
#include <android/os/IStoraged.h>
#include <android/os/BnVoldTaskListener.h>
#include <android/os/BnVoldListener.h>
#include <android/os/BnVoldMountCallback.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <atomic>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

using namespace android;
using namespace android::os;
using binder::Status;
#define CHECK(condition) do {if(!(condition)){fprintf(stderr,"API32_STORAGE_PROBE_FAILURE line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
class Volumes final : public BnVoldListener {
public:
    std::atomic<int> events{0};
    Status onDiskCreated(const std::string&,int32_t) override {events++;return Status::ok();}
    Status onDiskScanned(const std::string&) override {events++;return Status::ok();}
    Status onDiskMetadataChanged(const std::string&,int64_t,const std::string&,const std::string&) override {events++;return Status::ok();}
    Status onDiskDestroyed(const std::string&) override {events++;return Status::ok();}
    Status onVolumeCreated(const std::string&,int32_t,const std::string&,const std::string&,int32_t) override {events++;return Status::ok();}
    Status onVolumeStateChanged(const std::string&,int32_t) override {events++;return Status::ok();}
    Status onVolumeMetadataChanged(const std::string&,const std::string&,const std::string&,const std::string&) override {events++;return Status::ok();}
    Status onVolumePathChanged(const std::string&,const std::string&) override {events++;return Status::ok();}
    Status onVolumeInternalPathChanged(const std::string&,const std::string&) override {events++;return Status::ok();}
    Status onVolumeDestroyed(const std::string&) override {events++;return Status::ok();}
};
class Task final : public BnVoldTaskListener {
public:
    std::atomic<int> result{INT_MIN};
    Status onStatus(int32_t,const PersistableBundle&) override {return Status::ok();}
    Status onFinished(int32_t value,const PersistableBundle& extras) override {
        result=extras.empty() && IPCThreadState::self()->getCallingUid()==0?value:INT_MAX;
        return Status::ok();
    }
};
class Mount final : public BnVoldMountCallback {
public:
    std::atomic<int> calls{0};
    Status onVolumeChecking(base::unique_fd,const std::string&,const std::string&,bool* result) override {
        calls++;*result=false;return Status::ok();
    }
};
bool directory(const char* path,uid_t uid,gid_t gid,mode_t mode) {
    struct stat st;return !lstat(path,&st) && S_ISDIR(st.st_mode) && st.st_uid==uid &&
        st.st_gid==gid && (st.st_mode&07777)==mode;
}
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);bool denied=argc==2 && !strcmp(argv[1],"denied");
    if(argc!=2 || (!denied && strcmp(argv[1],"check")) || getuid()!=0 ||
        syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL)return 64;
    int role=denied?39999:1000;if(setgid(role) || setuid(role))return 65;
    ProcessState::self()->startThreadPool();auto manager=defaultServiceManager();CHECK(manager);
    auto service=interface_cast<IVold>(manager->checkService(String16("vold")));
    auto stats=interface_cast<IStoraged>(manager->checkService(String16("storaged")));CHECK(service && stats);
    if(denied) {
        bool answer=true;int32_t perf=-1;base::unique_fd fd;
        CHECK(service->supportsCheckpoint(&answer).exceptionCode()==Status::EX_SECURITY && answer);
        CHECK(service->initUser0().exceptionCode()==Status::EX_SECURITY);
        CHECK(service->monitor().exceptionCode()==Status::EX_SECURITY);
        CHECK(service->mountAppFuse(39999,1,&fd).exceptionCode()==Status::EX_SECURITY && fd.get()<0);
        CHECK(stats->getRecentPerf(&perf).exceptionCode()==Status::EX_SECURITY && perf==-1);
        puts("API32_STORAGE_ROLE_DENIED_PASS uid=39999 vold=denied storaged=denied fd=not_returned");return 0;
    }
    CHECK(IInterface::asBinder(service)->getInterfaceDescriptor()==String16("android.os.IVold"));
    CHECK(IInterface::asBinder(stats)->getInterfaceDescriptor()==String16("android.os.IStoraged"));
    for(auto method:{&IVold::supportsCheckpoint,&IVold::supportsBlockCheckpoint,&IVold::supportsFileCheckpoint,
        &IVold::needsCheckpoint,&IVold::needsRollback,&IVold::isCheckpointing,&IVold::isConvertibleToFbe,&IVold::incFsEnabled}) {
        bool value=true;CHECK((service.get()->*method)(&value).isOk() && !value);
    }
    CHECK(service->commitChanges().isOk() && service->prepareCheckpoint().isOk() && service->markBootAttempt().isOk());
    CHECK(service->startCheckpoint(1).exceptionCode()==Status::EX_UNSUPPORTED_OPERATION);
    int32_t encryption=-1;CHECK(service->fdeComplete(&encryption).isOk() && encryption==IVold::ENCRYPTION_STATE_NONE);
    CHECK(service->monitor().isOk() && service->initUser0().isOk());
    CHECK(service->onUserAdded(0,0).isOk() && service->onUserStarted(0).isOk());
    CHECK(service->createUserKey(0,0,false).isOk() && service->unlockUserKey(0,0,"!","!").isOk());
    CHECK(service->prepareUserStorage(std::nullopt,0,0,3).isOk());
    CHECK(directory("/data/system_de/0",1000,1000,0770));
    CHECK(directory("/data/system_ce/0",1000,1000,0770));
    CHECK(directory("/data/user_de/0",1000,1000,0771));
    CHECK(directory("/data/vendor_de/0",0,0,0771));
    CHECK(directory("/data/media/0",1023,1023,02770));
    CHECK(directory("/data/misc_ce/0/apexdata/com.android.wifi",0,1000,0771));
    CHECK(service->prepareUserStorage(std::nullopt,10,0,3).exceptionCode()==Status::EX_ILLEGAL_ARGUMENT);
    CHECK(service->prepareUserStorage(std::nullopt,0,1,3).exceptionCode()==Status::EX_ILLEGAL_ARGUMENT);
    CHECK(service->prepareUserStorage(std::nullopt,0,0,8).exceptionCode()==Status::EX_ILLEGAL_ARGUMENT);
    CHECK(service->prepareUserStorage(std::string("../../outside"),0,0,3).exceptionCode()==Status::EX_UNSUPPORTED_OPERATION);
    CHECK(service->unlockUserKey(0,0,"not-a-token","!").exceptionCode()==Status::EX_ILLEGAL_ARGUMENT);
    CHECK(service->destroyUserStorage(std::nullopt,0,3).exceptionCode()==Status::EX_UNSUPPORTED_OPERATION);
    CHECK(service->addAppIds({"invalid/name"},{10001}).exceptionCode()==Status::EX_ILLEGAL_ARGUMENT);
    CHECK(service->addAppIds({"dev.kreijstal.pocket.storage"},{10001}).isOk());
    uint64_t nonce;CHECK(getrandom(&nonce,sizeof(nonce),0)==sizeof(nonce));
    char names[2][180];constexpr char payload[]="real Android 12L private storage\n";
    for(int i=0;i<2;i++) {
        snprintf(names[i],sizeof(names[i]),"/data/misc_%s/0/apexdata/com.android.wifi/.pocket-vold-test-%016llx",
            i?"ce":"de",static_cast<unsigned long long>(nonce));
        int fd=open(names[i],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);CHECK(fd>=0);
        CHECK(write(fd,payload,sizeof(payload))==sizeof(payload) && !fsync(fd) && !close(fd));
    }
    sp<Volumes> listener=new Volumes;CHECK(service->setListener(listener).isOk());
    CHECK(service->reset().isOk() && service->initUser0().isOk());
    for(const auto& path:names) {
        int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);CHECK(fd>=0);char bytes[sizeof(payload)];struct stat st;
        CHECK(read(fd,bytes,sizeof(bytes))==sizeof(bytes) && !memcmp(bytes,payload,sizeof(bytes)) &&
            !fstat(fd,&st) && st.st_uid==1000 && !close(fd) && !unlink(path));
    }
    CHECK(listener->events==0); // No fictitious mounted external volume.
    sp<Mount> mount=new Mount;
    CHECK(service->mount("emulated",3,0,mount).exceptionCode()==Status::EX_UNSUPPORTED_OPERATION && mount->calls==0);
    base::unique_fd fuse;CHECK(service->mountAppFuse(1000,1,&fuse).exceptionCode()==Status::EX_UNSUPPORTED_OPERATION && fuse.get()<0);
    sp<Task> task=new Task;CHECK(service->fstrim(0,task).isOk());
    for(int i=0;i<200 && task->result==INT_MIN;i++)usleep(10000);
    CHECK(task->result==-EOPNOTSUPP);
    CHECK(service->fstrim(2,task).exceptionCode()==Status::EX_ILLEGAL_ARGUMENT);
    CHECK(stats->onUserStarted(0).isOk());int32_t perf=-1;CHECK(stats->getRecentPerf(&perf).isOk() && perf==0);
    CHECK(stats->onUserStopped(0).isOk());
    puts("API32_STORAGE_PASS typed_platform=12L user0_de_ce_directories=1 file_io=real reset_preserves_data=1 metadata=verified invalid_rejected=1 encryption=none checkpoint=unsupported external_fuse=unsupported");
    puts("API32_STORAGE_MAINTENANCE_PASS callback=typed status=unsupported bundle=decoded");
    return 0;
}
