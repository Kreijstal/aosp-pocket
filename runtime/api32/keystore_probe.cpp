// SPDX-License-Identifier: GPL-3.0-only
#include <android/security/maintenance/IKeystoreMaintenance.h>
#include <android/system/keystore2/IKeystoreService.h>
#include <android/hardware/keymaster/4.1/IKeymasterDevice.h>
#include <android/hidl/manager/1.0/IServiceManager.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <hidl/ServiceManagement.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

using namespace android;
using namespace android::system::keystore2;
using android::security::maintenance::IKeystoreMaintenance;
using android::security::maintenance::UserState;
#define CHECK(c) do {if(!(c)){fprintf(stderr,"API32_KEYSTORE_PROBE_FAILED line=%d\n",__LINE__);return 1;}}while(0)
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);
    CHECK(syscall(__NR_getpid,0x504c1001UL)==0x504c1002UL && getuid()==0 && argc==2);
    bool denied=!strcmp(argv[1],"denied");bool hal=!strcmp(argv[1],"hal");
    CHECK(denied || hal || !strcmp(argv[1],"ready"));
    CHECK(!setgid(denied?39999:1000) && !setuid(denied?39999:1000));
    if(hal) {
        using namespace hardware::keymaster;
        sp<V4_1::IKeymasterDevice> device;
        for(unsigned n=0;n<200 && !device;n++) {device=V4_1::IKeymasterDevice::tryGetService();if(!device)usleep(50000);}
        CHECK(device && device->isRemote());bool software=false;
        CHECK(device->getHardwareInfo([&](auto level,const auto& name,const auto& author){
            software=level==V4_0::SecurityLevel::SOFTWARE && name.size()>0 && author.size()>0;
        }).isOk());CHECK(software);
        auto legacy=V4_0::IKeymasterDevice::tryGetService();CHECK(legacy && legacy->isRemote());
        puts("API32_SOFTWARE_KEYMASTER_PASS stock_factory=4.1 security_level=SOFTWARE alias_4_0=remote");return 0;
    }
    ProcessState::self()->startThreadPool();auto manager=defaultServiceManager();CHECK(manager);
    sp<IBinder> binder;
    for(unsigned n=0;n<300 && !binder;n++) {binder=manager->checkService(String16("android.system.keystore2.IKeystoreService/default"));if(!binder)usleep(50000);}
    CHECK(binder && binder->pingBinder()==OK);
    auto service=interface_cast<IKeystoreService>(binder);CHECK(service);
    // keystore2 publishes its public interface before the maintenance service.
    // Wait for the interface we actually use, not just the first published one.
    sp<IBinder> maintenanceBinder;
    for(unsigned n=0;n<300 && !maintenanceBinder;n++) {
        maintenanceBinder=manager->checkService(String16("android.security.maintenance"));
        if(!maintenanceBinder)usleep(50000);
    }
    CHECK(maintenanceBinder && maintenanceBinder->pingBinder()==OK);
    auto maintenance=interface_cast<IKeystoreMaintenance>(maintenanceBinder);CHECK(maintenance);
    UserState state{};auto status=maintenance->getState(0,&state);
    if(denied) {
        CHECK(status.exceptionCode()==binder::Status::EX_SERVICE_SPECIFIC && status.serviceSpecificErrorCode()==6);
        std::vector<KeyDescriptor> entries;
        status=service->listEntries(Domain::APP,1000,&entries);
        // APP ignores the supplied namespace and always selects the caller.
        // A different UID argument is not a request for another UID's keys.
        CHECK(status.isOk());
        for(const auto& entry:entries)CHECK(entry.domain==Domain::APP && entry.nspace==39999);
        status=service->listEntries(Domain::SELINUX,103,&entries);
        CHECK(status.exceptionCode()==binder::Status::EX_SERVICE_SPECIFIC && status.serviceSpecificErrorCode()==6);
        status=maintenance->deleteAllKeys();
        CHECK(status.exceptionCode()==binder::Status::EX_SERVICE_SPECIFIC && status.serviceSpecificErrorCode()==6);
        CHECK(service->listEntries(Domain::APP,-1,&entries).isOk());
        auto hardwareManager=hardware::defaultServiceManager();CHECK(hardwareManager);
        auto raw=hardwareManager->get("android.hardware.keymaster@4.1::IKeymasterDevice","default");
        CHECK(raw.isOk() && !raw.withDefault(nullptr));
        puts("API32_KEYSTORE_ROLE_DENIED_PASS uid=39999 maintenance=denied privileged_namespace=denied destructive_operation=denied app_namespace=caller_scoped raw_hal=denied");
    } else {
        if(!status.isOk())fprintf(stderr,"API32_KEYSTORE_STATE_FAILED %s\n",status.toString8().c_str());
        CHECK(status.isOk() && (state==UserState::UNINITIALIZED || state==UserState::LSKF_UNLOCKED || state==UserState::LSKF_LOCKED));
        CHECK(service->getInterfaceVersion()==1);
        CHECK(service->getInterfaceHash()=="19e8b65277839bad0ab335c781e3c652324920ce");
        CHECK(manager->isDeclared(String16("android.system.keystore2.IKeystoreService/default")));
        auto instances=manager->getDeclaredInstances(String16("android.system.keystore2.IKeystoreService"));
        CHECK(instances.size()==1 && instances[0]==String16("default"));
        CHECK(!manager->isDeclared(String16("android.hardware.security.keymint.IKeyMintDevice/default")));
        printf("API32_KEYSTORE_CONTROL_PASS stock_daemon=12L aidl=1 hash=verified maintenance=typed user_state=%d\n",static_cast<int>(state));
    }
    return 0;
}
