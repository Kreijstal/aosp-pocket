// SPDX-License-Identifier: GPL-3.0-only
// Android 12L native registry/client transport probe; original API 32 Parcel APIs.
#include "binder_bridge.h"
#include "service_registry.h"
#include "binder_token.h"
#include <binder/Binder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <binder/ProcessState.h>
#include <utils/String8.h>
#include <map>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/system_properties.h>
#ifndef POCKET_GUEST_API
#define POCKET_GUEST_API 32
#endif
#define POCKET_API_STRING_INNER(value) #value
#define POCKET_API_STRING(value) POCKET_API_STRING_INNER(value)
#define POCKET_API_LABEL "API" POCKET_API_STRING(POCKET_GUEST_API)
#define POCKET_ECHO_SERVICE "pocket.api" POCKET_API_STRING(POCKET_GUEST_API) ".echo"
#define POCKET_FD_VALUE "api" POCKET_API_STRING(POCKET_GUEST_API) "fd"

using namespace android;
namespace {
class Echo final : public BBinder {
    status_t dump(int fd,const Vector<String16>&) override {
        auto* thread=IPCThreadState::self();
        unsigned uid=thread->getCallingUid();int pid=thread->getCallingPid();
        bool allowed=uid==0 || uid==1000;
        dprintf(fd,"uid=%u pid=%d allowed=%d\n",uid,pid,allowed);
        return allowed?NO_ERROR:PERMISSION_DENIED;
    }
    status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
        if (code == FIRST_CALL_TRANSACTION+1) {
            if (!data.enforceInterface(String16("dev.kreijstal.pocket.Echo"))) return PERMISSION_DENIED;
            IPCThreadState* thread=IPCThreadState::self();
            uint32_t event[]={static_cast<uint32_t>(thread->getCallingUid()),
                static_cast<uint32_t>(thread->getCallingPid()),static_cast<uint32_t>(thread->getCallingWorkSourceUid()),
                static_cast<uint32_t>(thread->getStrictModePolicy())};
            int fd=data.readFileDescriptor();
            return fd>=0 && pwrite(fd,event,sizeof(event),0)==sizeof(event) ? NO_ERROR : BAD_VALUE;
        }
        if (code != FIRST_CALL_TRANSACTION) return BBinder::onTransact(code, data, reply, flags);
        if (!data.enforceInterface(String16("dev.kreijstal.pocket.Echo"))) return PERMISSION_DENIED;
        int32_t input = data.readInt32();
        int fd = data.readFileDescriptor();
        char bytes[8] = {};
        if (fd < 0 || pread(fd, bytes, 7, 0) != 7 || strcmp(bytes, POCKET_FD_VALUE)) return BAD_VALUE;
        reply->writeInt32(input + 1);
        reply->writeInt32(getpid());
        reply->writeInt32(getuid());
        reply->writeInt32(IPCThreadState::self()->getCallingUid());
        reply->writeInt32(IPCThreadState::self()->getCallingPid());
#if POCKET_GUEST_API == 32
        reply->writeInt32(IPCThreadState::self()->getCallingWorkSourceUid());
        reply->writeInt32(const_cast<Parcel&>(data).readCallingWorkSourceUid());
#endif
        return NO_ERROR;
    }
};


}

#ifdef POCKET_CORE_TEST_ECHO
android::sp<android::IBinder> pocketCreateCoreTestEcho32() {return new Echo;}
#endif
int main(int argc, char** argv) {
    setbuf(stdout, nullptr);
    if (argc != 2) return 64;
    char sdk[PROP_VALUE_MAX];
    __system_property_get("ro.build.version.sdk", sdk);
    printf(POCKET_API_LABEL "_START mode=%s pid=%d uid=%d sdk=%s\n", argv[1], getpid(), getuid(), sdk);
    if (strcmp(sdk, POCKET_API_STRING(POCKET_GUEST_API))) return 65;
#if POCKET_GUEST_API == 32
    if (!strcmp(argv[1], "client-virtual") && (setgid(39999) || setuid(39999))) return 66;
    if (!strcmp(argv[1], "client-system") && (setgid(1000) || setuid(1000))) return 66;
#endif
    ProcessState::self()->startThreadPool();
    if (!strcmp(argv[1], "manager")) {
        sp<IBinder> manager = pocket::createServiceRegistry32();
        status_t result = pocket::publishManager(manager);
        printf(POCKET_API_LABEL "_PUBLISH status=%d\n", result);
        if (result != NO_ERROR) return 1;
        // The stock API 32 client must discover this published registry.
        sp<IServiceManager> sm = defaultServiceManager();
        if (!sm || sm->addService(String16(POCKET_ECHO_SERVICE), new Echo) != NO_ERROR) return 2;
        puts(POCKET_API_LABEL "_READY");
        IPCThreadState::self()->joinThreadPool();
        return 0;
    }
    const bool virtualClient = !strcmp(argv[1], "client-virtual") || !strcmp(argv[1], "client-system");
    if (strcmp(argv[1], "client") && !virtualClient) return 64;
    sp<IServiceManager> sm = defaultServiceManager();
    if (!sm || sm->checkService(String16("pocket.api32.absent")) ||
        sm->isDeclared(String16("pocket.api32.absent")) ||
        sm->getDeclaredInstances(String16("pocket.api32.absent")).size() ||
        sm->updatableViaApex(String16("pocket.api32.absent")).has_value()) return 9;
    Vector<String16> names = sm->listServices();
    bool listed = false;
    for (size_t i=0; i<names.size(); ++i) if (names[i] == String16(POCKET_ECHO_SERVICE)) listed = true;
    if (!listed) return 10;
    puts("API32_REGISTRY_TYPED_RESULTS_PASS");
    sp<IBinder> echo = sm ? sm->checkService(String16(POCKET_ECHO_SERVICE)) : nullptr;
    if (!echo) return 3;
    Parcel request, reply;
#if POCKET_GUEST_API == 32
    const int64_t workSourceToken=IPCThreadState::self()->setCallingWorkSourceUid(32017);
#endif
    request.writeInterfaceToken(String16("dev.kreijstal.pocket.Echo"));
#if POCKET_GUEST_API == 32
    if (request.readCallingWorkSourceUid()!=32017 || !request.replaceCallingWorkSourceUid(32018) ||
        request.readCallingWorkSourceUid()!=32018) return 7;
    IPCThreadState::self()->restoreCallingWorkSource(workSourceToken);
#endif
    request.writeInt32(POCKET_GUEST_API-1);
    FILE* file = tmpfile();
    if (!file) return 4;
    if (fwrite(POCKET_FD_VALUE, 1, 7, file) != 7 || fflush(file)) return 5;
    request.writeFileDescriptor(fileno(file));
    status_t result = echo->transact(IBinder::FIRST_CALL_TRANSACTION, request, &reply);
    int answer = reply.readInt32(), server = reply.readInt32(), uid = reply.readInt32();
    int callerUid = reply.readInt32(), callerPid = reply.readInt32();
#if POCKET_GUEST_API == 32
    int workSource=reply.readInt32(), parcelWorkSource=reply.readInt32();
    printf("API32_WORKSOURCE thread_uid=%d parcel_uid=%d\n",workSource,parcelWorkSource);
    if (workSource!=32018 || parcelWorkSource!=32018) return 8;
#endif
    if (ftruncate(fileno(file),0)) return 11;
    const int64_t asyncWorkSource=IPCThreadState::self()->setCallingWorkSourceUid(32019);
    Parcel async;
    async.writeInterfaceToken(String16("dev.kreijstal.pocket.Echo"));
    IPCThreadState::self()->restoreCallingWorkSource(asyncWorkSource);
    async.writeFileDescriptor(fileno(file));
    if (echo->transact(IBinder::FIRST_CALL_TRANSACTION+1,async,nullptr,IBinder::FLAG_ONEWAY)!=NO_ERROR) return 12;
    uint32_t event[4]={};
    for (int count=0;count<200;count++) {
        if (pread(fileno(file),event,sizeof(event),0)==sizeof(event)) break;
        usleep(20000);
    }
#if POCKET_GUEST_API == 32
    if(ftruncate(fileno(file),0) || lseek(fileno(file),0,SEEK_SET)<0)return 14;
    Vector<String16> dumpArgs;
    status_t dumped=echo->dump(fileno(file),dumpArgs);
    char dumpText[160]={};
    ssize_t dumpSize=pread(fileno(file),dumpText,sizeof(dumpText)-1,0);
    unsigned dumpUid=~0u;int dumpPid=-1,dumpAllowed=-1;
    bool allowed=getuid()==0 || getuid()==1000;
    if(dumpSize<=0 || sscanf(dumpText,"uid=%u pid=%d allowed=%d",&dumpUid,&dumpPid,&dumpAllowed)!=3 ||
        dumpUid!=getuid() || dumpPid!=getpid() || dumpAllowed!=static_cast<int>(allowed) ||
        dumped!=(allowed?NO_ERROR:PERMISSION_DENIED)) {
        printf("API32_DUMP_IDENTITY_FAILED status=%d state=%s",dumped,dumpText);return 15;
    }
    printf("API32_DUMP_IDENTITY_PASS uid=%u pid=kernel_matched allowed=%d token=absent\n",dumpUid,dumpAllowed);
#endif
    fclose(file);
    printf("API32_ONEWAY_CALLER uid=%u pid=%u worksource=%u strict=%u\n",event[0],event[1],event[2],event[3]);
    if (event[0]!=getuid() || event[1]!=static_cast<uint32_t>(getpid()) || event[2]!=32019 || event[3]!=0) return 13;
    puts("API32_ONEWAY_CALLER_PASS");
    printf(POCKET_API_LABEL "_RESULT status=%d answer=%d server_pid=%d client_pid=%d server_uid=%d client_uid=%d fd=" POCKET_FD_VALUE " caller_uid=%d caller_pid=%d\n",
           result, answer, server, getpid(), uid, getuid(), callerUid, callerPid);
    return result == NO_ERROR && answer == POCKET_GUEST_API && server != getpid() &&
        (virtualClient ? uid == 0 && (getuid() == 39999 || getuid() == 1000) : static_cast<uid_t>(uid) == getuid())
        && static_cast<uid_t>(callerUid) == getuid() && callerPid == getpid() ? 0 : 6;
}
