// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../../api32/external_files_rpc.h"
#ifdef POCKET_EXTERNAL_PATH32
#include "../../api32/external_open_rpc.h"
#endif
static void mediaProxy(int control,pocket::fuse32::Files& files,const char* run,const std::string& package) {
    using namespace pocket::fuse32;
    char* end=nullptr;const char* parentText=getenv("POCKET_PARENT_PID");REQUIRE(parentText);
    long parent=strtol(parentText,&end,10);REQUIRE(end && !*end && parent>0 && parent<=INT32_MAX);
    uid_t hostUid;REQUIRE(kernelUid(&hostUid)==0);
    Identity actor{getuid(),getgid(),static_cast<uint32_t>(syscall(__NR_gettid))};
    (void)package;
#ifndef POCKET_EXTERNAL_PATH32
    std::string path="/0/Android/media/"+package+"/canary";Files::Opened cached{},direct{};
    int opened=files.open(path,O_RDWR,actor,&cached);printf("MEDIA_PROXY_OPEN_RESULT=%d\n",opened);REQUIRE(opened==0);
    REQUIRE(files.open(path,O_RDONLY|O_DIRECT,actor,&direct)==0);
    std::vector<char> invalid;REQUIRE(files.read(cached.slot,cached.cookie+999,0,1,actor,&invalid)==-EBADF);
#endif
    char parentPath[64],parentStat[4096];snprintf(parentPath,sizeof(parentPath),"/proc/%ld/stat",parent);
    ssize_t parentBytes=readIdentityFile(parentPath,parentStat,sizeof(parentStat));
    printf("MEDIA_PROXY_PARENT_STAT bytes=%zd start=%llu host_uid=%u\n",parentBytes,
        (unsigned long long)(parentBytes>0?pocket_process_start(parentStat,parentBytes):0),hostUid);
    const char* startText=getenv("POCKET_API32_HOST_START");REQUIRE(startText && *startText>='0' && *startText<='9');
    errno=0;unsigned long long parentStart=strtoull(startText,&end,10);REQUIRE(!errno && end && !*end && parentStart>0);
    HostFiles broker(files,parent,hostUid,parentStart);int listening=broker.start("/dev/socket/pocket_ext");
    printf("MEDIA_PROXY_BROKER_RESULT=%d parent=%ld self=%d app_parent=%d\n",listening,parent,getpid(),getppid());REQUIRE(listening==0);
#ifdef POCKET_EXTERNAL_PATH32
    AppFiles apps(files,parent,hostUid,parentStart);REQUIRE(apps.start("/dev/socket/pocket_efs","/dev/socket/pocket_xfd","/.pocket-external-ready")==0);
    char info[1024];int size=snprintf(info,sizeof(info),"{\"runId\":\"%s\",\"pid\":%d,\"pathOpens\":true}\n",run,getpid());
#else
    char info[1024];int size=snprintf(info,sizeof(info),"{\"runId\":\"%s\",\"pid\":%d,\"files\":[{\"slot\":%u,\"cookie\":%llu,\"flags\":%u,\"mode\":2},{\"slot\":%u,\"cookie\":%llu,\"flags\":%u,\"mode\":0}]}\n",run,getpid(),cached.slot,(unsigned long long)cached.cookie,cached.flags,direct.slot,(unsigned long long)direct.cookie,direct.flags);
#endif
    REQUIRE(size>0 && size<(int)sizeof(info));int output=open("/tmp/media-proxy-ready.tmp",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);REQUIRE(output>=0);
    REQUIRE(write(output,info,size)==size && close(output)==0 && rename("/tmp/media-proxy-ready.tmp","/tmp/media-proxy-ready")==0);
#ifdef POCKET_EXTERNAL_PATH32
    for(int i=0;;i++) {
        char ready[33]{};ssize_t n=readIdentityFile("/tmp/media-proxy-host-ready",ready,sizeof(ready));
        if(n==32 && !memcmp(ready,run,32))break;
        REQUIRE(i<400);usleep(50000);
    }
#endif
    char tag=0;REQUIRE(write(control,"P",1)==1 && read(control,&tag,1)==1 && tag=='R');
#ifdef POCKET_EXTERNAL_PATH32
    apps.stop();REQUIRE(apps.opened==3 && apps.denied>=2 && apps.statQueries>=1);
    printf("MEDIA_PATH32_PASS opens=%u denied=%u statfs=%u\n",apps.opened.load(),apps.denied.load(),apps.statQueries.load());
#endif
    broker.stop();REQUIRE(files.count()==0 && broker.denied>=1 && broker.reads>=2 && broker.writes>=1);
    printf("MEDIA_PROXY32_PASS opener_uid=%u authenticated_data_calls=verified denied_peers=%u reads=%u writes=%u remaining_files=0\n",actor.uid,broker.denied.load(),broker.reads.load(),broker.writes.load());
}
