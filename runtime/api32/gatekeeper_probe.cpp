// SPDX-License-Identifier: GPL-3.0-only
// Stock 12L daemon + stock emulator software HAL. No TEE/token-MAC claim.
#include <android/service/gatekeeper/IGateKeeperService.h>
#include <gatekeeper/GateKeeperResponse.h>
#include <gatekeeper/password_handle.h>
#include <hardware/hw_auth_token.h>
#include <android/hardware/gatekeeper/1.0/IGatekeeper.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <endian.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <optional>
#include <algorithm>
#include <signal.h>

using namespace android;
using namespace android::service::gatekeeper;
#define CHECK(c) do{if(!(c)){fprintf(stderr,"API32_GATEKEEPER_PROBE_FAILED line=%d\n",__LINE__);return 1;}}while(0)
namespace {
sp<IGateKeeperService> findGatekeeper() {
    ProcessState::self()->startThreadPool();auto manager=defaultServiceManager();if(!manager)return nullptr;
    for(unsigned n=0;n<300;n++) {
        auto binder=manager->checkService(String16("android.service.gatekeeper.IGateKeeperService"));
        if(binder && binder->pingBinder()==OK)return interface_cast<IGateKeeperService>(binder);
        usleep(50000);
    }
    return nullptr;
}
struct Cleanup {
    sp<IGateKeeperService> daemon;int user;
    ~Cleanup(){if(daemon)daemon->clearSecureUserId(user);}
};
struct DenialRequest {int user;int64_t sid;};
struct DenialChild {
    pid_t pid=0;int pipeFd=-1;
    ~DenialChild(){if(pipeFd>=0)close(pipeFd);if(pid>0 && waitpid(pid,nullptr,WNOHANG)==0){kill(pid,SIGKILL);waitpid(pid,nullptr,0);}}
};
}
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);CHECK(getuid()==0 && syscall(__NR_getpid,0x504c1001UL)==0x504c1002UL);
    bool denied=argc==4 && !strcmp(argv[1],"denied");
    bool sharedFrontend=argc==2 && !strcmp(argv[1],"shared-frontend");
    CHECK(denied || sharedFrontend || argc==1);
    DenialChild child;
    if(!denied) {
        int pipeFd[2];CHECK(!pipe(pipeFd));child.pid=fork();CHECK(child.pid>=0);
        if(!child.pid) {
            close(pipeFd[1]);DenialRequest request{};
            if(read(pipeFd[0],&request,sizeof(request))!=sizeof(request))_exit(126);close(pipeFd[0]);
            char user[32],sid[32];snprintf(user,sizeof(user),"%d",request.user);snprintf(sid,sizeof(sid),"%lld",static_cast<long long>(request.sid));
            execl(argv[0],argv[0],"denied",user,sid,nullptr);_exit(127);
        }
        close(pipeFd[0]);child.pipeFd=pipeFd[1];
    }
    CHECK(!setgid(denied?39999:1000) && !setuid(denied?39999:1000));
    auto daemon=findGatekeeper();CHECK(daemon);
    if(denied) {
        int user=atoi(argv[2]);int64_t expected=strtoll(argv[3],nullptr,10),sid=0;CHECK(user>0 && expected);
        CHECK(daemon->getSecureUserId(user,&sid).isOk() && sid==expected); // Public in stock 12L.
        CHECK(daemon->clearSecureUserId(user).isOk());
        CHECK(daemon->getSecureUserId(user,&sid).isOk() && sid==expected);
        GateKeeperResponse response;std::vector<uint8_t> password{1,2,3,4};
        CHECK(daemon->enroll(user,std::nullopt,std::nullopt,password,&response).isOk() && response.response_code()==ResponseCode::ERROR);
        auto hal=hardware::gatekeeper::V1_0::IGatekeeper::tryGetService();CHECK(!hal);
        puts("API32_GATEKEEPER_ROLE_DENIED_PASS uid=39999 enroll=error clear_sid=unchanged public_sid=readable raw_hal=denied");return 0;
    }
    auto hal=hardware::gatekeeper::V1_0::IGatekeeper::tryGetService();CHECK(hal && hal->isRemote());
    // A normal framework boot owns setup/coldboot. Never reset HAL state while
    // validating a retained profile containing other users' credentials.
    if(!sharedFrontend)CHECK(daemon->reportDeviceSetupComplete().isOk());
    const int user=10000+(getpid()%500000);int64_t sid=0;CHECK(daemon->getSecureUserId(user,&sid).isOk() && !sid);
    Cleanup cleanup{daemon,user};std::vector<uint8_t> password(32);CHECK(getrandom(password.data(),password.size(),0)==static_cast<ssize_t>(password.size()));
    GateKeeperResponse response;CHECK(daemon->enroll(user,std::nullopt,std::nullopt,password,&response).isOk());
    CHECK(response.response_code()==ResponseCode::OK && response.payload().size()==sizeof(::gatekeeper::password_handle_t));
    auto handle=response.payload();::gatekeeper::password_handle_t parsed;memcpy(&parsed,handle.data(),sizeof(parsed));
    CHECK(!parsed.hardware_backed && parsed.user_id && daemon->getSecureUserId(user,&sid).isOk() && static_cast<uint64_t>(sid)==parsed.user_id);
    auto wrong=password;wrong[0]^=1;
    CHECK(daemon->verifyChallenge(user,17,handle,wrong,&response).isOk() && response.response_code()==ResponseCode::ERROR);
    const int64_t challenge=0x123456789aLL;
    CHECK(daemon->verifyChallenge(user,challenge,handle,password,&response).isOk() && response.response_code()==ResponseCode::OK);
    CHECK(response.payload().size()==sizeof(hw_auth_token_t));hw_auth_token_t token;memcpy(&token,response.payload().data(),sizeof(token));
    CHECK(token.challenge==static_cast<uint64_t>(challenge) && token.user_id==parsed.user_id && be32toh(token.authenticator_type)==HW_AUTH_PASSWORD && be64toh(token.timestamp)>0);
    // The SDK software HAL caches a successful password hash by SID. Its
    // documented fast path does not recheck signature bytes for that same SID.
    // Alter the identity-bearing field: the password must not authenticate a
    // different SID, including after warming that cache.
    auto tampered=handle;tampered[offsetof(::gatekeeper::password_handle_t,user_id)]^=1;
    CHECK(daemon->verifyChallenge(user,challenge,tampered,password,&response).isOk() && response.response_code()==ResponseCode::ERROR);
    if(sharedFrontend) {
        std::vector<uint8_t> next(32);CHECK(getrandom(next.data(),next.size(),0)==static_cast<ssize_t>(next.size()));
        CHECK(daemon->enroll(user,handle,password,next,&response).isOk() && response.response_code()==ResponseCode::OK);
        auto replacement=response.payload();CHECK(replacement.size()==sizeof(::gatekeeper::password_handle_t));
        CHECK(daemon->getSecureUserId(user,&sid).isOk() && static_cast<uint64_t>(sid)==parsed.user_id);
        // The stock software HAL's fast cache is keyed by SID, not handle.
        // Authenticate the replacement first to refresh that existing cache;
        // this is a frontend integration test, not a stronger HAL claim.
        CHECK(daemon->verifyChallenge(user,challenge,replacement,next,&response).isOk() && response.response_code()==ResponseCode::OK);
        CHECK(response.payload().size()==sizeof(hw_auth_token_t));
        memcpy(&token,response.payload().data(),sizeof(token));
        CHECK(token.user_id==parsed.user_id && token.challenge==static_cast<uint64_t>(challenge));
        CHECK(daemon->verifyChallenge(user,challenge,replacement,password,&response).isOk() && response.response_code()==ResponseCode::ERROR);
        std::fill(next.begin(),next.end(),0);
        puts("API32_GATEKEEPER_REENROLL_PASS changed_password=verified old_password_after_new_auth=rejected sid=retained normal_boot_setup=untouched stock_sid_cache=retained");
    }
    DenialRequest request{user,sid};CHECK(write(child.pipeFd,&request,sizeof(request))==sizeof(request));
    close(child.pipeFd);child.pipeFd=-1;
    int status=0;pid_t waited=waitpid(child.pid,&status,0);CHECK(waited==child.pid);child.pid=0;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(daemon->clearSecureUserId(user).isOk() && daemon->getSecureUserId(user,&sid).isOk() && sid==0);
    cleanup.daemon.clear();
    std::fill(password.begin(),password.end(),0);std::fill(wrong.begin(),wrong.end(),0);
    printf("API32_GATEKEEPER_PASS stock_daemon=12L stock_software_hal=1 setup_report=%s enrollment=real wrong_password=rejected altered_sid=rejected challenge=matched sid_lifecycle=verified hardware_backed=0 emulator_token_mac=not_secure cleanup=1\n",sharedFrontend?"normal_boot_unchanged":"real");
    return 0;
}
