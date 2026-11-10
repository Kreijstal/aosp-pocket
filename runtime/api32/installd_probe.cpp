// SPDX-License-Identifier: GPL-3.0-only
// Real stock-installd operations through the pinned Android 12L typed AIDL.
#include <android/os/IInstalld.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <functional>
#include "dexopt.h"

using namespace android;
using os::IInstalld;
using os::CreateAppDataArgs;
using os::CreateAppDataResult;
namespace {
sp<IInstalld> service() {
    ProcessState::self()->startThreadPool();
    auto manager=defaultServiceManager();
    if(!manager)return nullptr;
    for(unsigned i=0;i<200;i++) {
        auto binder=manager->checkService(String16("installd"));
        if(binder)return interface_cast<IInstalld>(binder);
        usleep(50000);
    }
    return nullptr;
}
bool systemCall(const std::function<bool(const sp<IInstalld>&)>& operation) {
    // Fork before opening a Binder driver in the parent. Each system-role
    // client gets its own connection; the root parent inspects real files.
    pid_t pid=fork();if(pid<0)return false;
    if(!pid) {
        if(setgid(1000) || setuid(1000))_exit(90);
        auto target=service();_exit(target && operation(target)?0:91);
    }
    int status;
    while(waitpid(pid,&status,0)<0)if(errno!=EINTR)return false;
    return WIFEXITED(status) && !WEXITSTATUS(status);
}
bool ok(const binder::Status& status,const char* operation) {
    if(status.isOk())return true;
    fprintf(stderr,"API32_INSTALLD_ERROR operation=%s status=%s\n",operation,status.toString8().c_str());
    return false;
}
std::string path(const CreateAppDataArgs& args,bool ce) {
    return std::string(ce?"/data/user/0/":"/data/user_de/0/")+args.packageName;
}
bool directory(const std::string& name,int uid,int gid,mode_t mode) {
    struct stat st;
    bool result=!stat(name.c_str(),&st) && S_ISDIR(st.st_mode) && st.st_uid==unsigned(uid) &&
        st.st_gid==unsigned(gid) && (st.st_mode&07777)==mode;
    if(!result)fprintf(stderr,"API32_INSTALLD_DIRECTORY_FAILED path=%s errno=%d\n",name.c_str(),errno);
    return result;
}
bool dexopt(const char* source,bool postBoot,bool appWorker) {
    unsigned long long nonce;
    if(getrandom(&nonce,sizeof(nonce),0)!=sizeof(nonce))return false;
    char folder[128];snprintf(folder,sizeof(folder),"/data/app/pocket-api32-dexopt-%016llx",nonce);
    std::string codeDir=folder,apk=codeDir+"/base.apk",oat=codeDir+"/oat";
    if(mkdir(folder,0755) || chown(folder,1000,1000))return false;
    int input=open(source,O_RDONLY|O_CLOEXEC);
    int output=open(apk.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
    if(input<0 || output<0)return false;
    char buffer[8192];ssize_t count;
    while((count=read(input,buffer,sizeof(buffer)))>0) {
        ssize_t offset=0;
        while(offset<count) {
            ssize_t written=write(output,buffer+offset,count-offset);
            if(written<0 && errno==EINTR)continue;
            if(written<=0)return false;
            offset+=written;
        }
    }
    if(count<0 || fchown(output,1000,1000) || close(input) || close(output))return false;
    // A distinct request must exercise the post-boot task-profile path.
    const int flags=installd::DEXOPT_PUBLIC|(postBoot?installd::DEXOPT_BOOTCOMPLETE:0);
    const uid_t targetUid=appWorker?12347:1000;
    printf("API32_INSTALLD_DEXOPT_REQUEST boot_complete=%d flags=%d\n",postBoot?1:0,flags);
    printf("API32_INSTALLD_DEXOPT_IDENTITY caller_uid=1000 target_uid=%u\n",targetUid);
    bool compiled=systemCall([&](const auto& target){
        return ok(target->createOatDir(oat,"arm64"),"createOatDir") &&
            ok(target->dexopt(apk,targetUid,"dev.kreijstal.pocket.api32.dexopt","arm64",
                installd::DEX2OAT_FROM_SCRATCH,oat,flags,"verify",
                std::nullopt,"PCL[]",std::nullopt,false,32,std::nullopt,std::nullopt,"install"),"dexopt");
    });
    bool valid=compiled;
    for(const auto& artifact:{std::make_pair("base.odex","\177ELF"),std::make_pair("base.vdex","vdex")}) {
        std::string name=oat+"/arm64/"+artifact.first;
        struct stat st;char magic[4];int fd=open(name.c_str(),O_RDONLY|O_CLOEXEC);
        bool matches=fd>=0 && !fstat(fd,&st) && st.st_size>4 && st.st_uid==1000 &&
            st.st_gid==targetUid && (st.st_mode&07777)==0644 &&
            read(fd,magic,sizeof(magic))==sizeof(magic) && !memcmp(magic,artifact.second,sizeof(magic));
        if(fd>=0)close(fd);
        if(!matches)fprintf(stderr,"API32_INSTALLD_DEXOPT_ARTIFACT_FAILED path=%s errno=%d\n",name.c_str(),errno);
        valid=valid && matches;
    }
    bool removed=systemCall([&](const auto& target){return ok(target->rmPackageDir(codeDir),"rmPackageDir");});
    struct stat st;
    bool absent=lstat(folder,&st)==-1 && errno==ENOENT;
    printf("API32_INSTALLD_DEXOPT_CLEANUP removed=%d absent=%d\n",removed?1:0,absent?1:0);
    if(!valid || !removed || !absent)return false;
    printf("API32_INSTALLD_DEXOPT_OWNERSHIP uid=1000 gid=%u mode=0644\n",targetUid);
    printf("API32_INSTALLD_DEXOPT_PASS stock_dex2oat=1 filter=verify odex_elf=1 vdex=1 cleanup=1 boot_time=%d\n",postBoot?0:1);
    return true;
}
}
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);
    if(argc<2 || getuid()!=0)return 64;
    const bool appWorker=!strcmp(argv[1],"dexopt-postboot-app");
    const bool postBoot=appWorker || !strcmp(argv[1],"dexopt-postboot");
    const bool compile=postBoot || !strcmp(argv[1],"dexopt");
    if(argc!=2 && !(argc==3 && compile))return 64;
    if(compile)return dexopt(argc==3?argv[2]:"/system/etc/pocket-api32-smoke.apk",postBoot,appWorker)?0:10;
    if(!strcmp(argv[1],"denied")) {
        if(setgid(39999) || setuid(39999))return 65;
        auto target=service();
        if(!target || target->invalidateMounts().exceptionCode()!=binder::Status::EX_SECURITY)return 1;
        puts("API32_INSTALLD_ROLE_DENIED_PASS uid=39999 operation=invalidateMounts");return 0;
    }
    if(strcmp(argv[1],"check"))return 64;
    unsigned long long nonce;
    if(getrandom(&nonce,sizeof(nonce),0)!=sizeof(nonce))return 2;
    char prefix[96];snprintf(prefix,sizeof(prefix),"dev.kreijstal.pocket.installd.p%016llx",nonce);
    std::vector<CreateAppDataArgs> args;
    for(unsigned i=0;i<2;i++) {
        CreateAppDataArgs arg;
        arg.packageName=std::string(prefix)+(i?".second":".first");arg.userId=0;
        arg.flags=IInstalld::FLAG_STORAGE_CE|IInstalld::FLAG_STORAGE_DE;
        arg.appId=12345+i;arg.seInfo="default:targetSdkVersion=32";arg.targetSdkVersion=32;
        args.push_back(arg);
    }
    if(!systemCall([&](const auto& target){
        if(!ok(target->invalidateMounts(),"invalidateMounts"))return false;
        bool quota=false;if(!ok(target->isQuotaSupported(std::nullopt,&quota),"isQuotaSupported"))return false;
        std::vector<CreateAppDataResult> results;
        if(!ok(target->createAppDataBatched(args,&results),"createAppDataBatched") || results.size()!=args.size())return false;
        for(size_t i=0;i<results.size();i++) {
            struct stat st;
            if(results[i].exceptionCode || stat(path(args[i],true).c_str(),&st) ||
                results[i].ceDataInode!=int64_t(st.st_ino)) {
                fprintf(stderr,"API32_INSTALLD_CREATE_FAILED code=%d message=%s inode=%lld\n",results[i].exceptionCode,
                    results[i].exceptionMessage.c_str(),static_cast<long long>(results[i].ceDataInode));return false;
            }
        }
        CreateAppDataArgs invalid=args[0];invalid.packageName="invalid/name";
        CreateAppDataResult rejected;
        return ok(target->createAppData(invalid,&rejected),"createAppData invalid") && rejected.exceptionCode!=0;
    }))return 3;
    std::vector<int64_t> inodes;
    for(const auto& arg:args) {
        struct stat st;
        if(stat(path(arg,true).c_str(),&st))return 4;
        inodes.push_back(st.st_ino);
        for(bool ce:{false,true}) {
            auto folder=path(arg,ce);
            int cacheGid=arg.appId-10000+20000;
            if(!directory(folder,arg.appId,arg.appId,0700) ||
                !directory(folder+"/cache",arg.appId,cacheGid,02771) ||
                !directory(folder+"/code_cache",arg.appId,cacheGid,02771))return 4;
            int fd=open((folder+"/probe-data").c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
            if(fd<0)return 5;
            char bytes[4096];memset(bytes,0x32,sizeof(bytes));
            for(unsigned i=0;i<16;i++)if(write(fd,bytes,sizeof(bytes))!=sizeof(bytes))return 5;
            if(fchown(fd,arg.appId,arg.appId) || close(fd))return 5;
        }
    }
    puts("API32_INSTALLD_CREATE_PASS batched=2 typed_results=1 ce_de=1 ownership=1 cache_groups=1 invalid_name=rejected");
    if(!systemCall([&](const auto& target){
        for(size_t i=0;i<args.size();i++) {
            const auto& arg=args[i];std::vector<int64_t> sizes;
            if(!ok(target->getAppSize(std::nullopt,{arg.packageName},0,0,arg.appId,{inodes[i]},{},&sizes),"getAppSize") ||
                sizes.size()!=6 || sizes[1]<65536)return false;
            if(!ok(target->clearAppData(std::nullopt,arg.packageName,0,arg.flags,inodes[i]),"clearAppData"))return false;
        }
        return true;
    }))return 6;
    for(const auto& arg:args)for(bool ce:{false,true}) {
        if(!directory(path(arg,ce),arg.appId,arg.appId,0700))return 7;
        struct stat st;
        if(lstat((path(arg,ce)+"/probe-data").c_str(),&st)!=-1 || errno!=ENOENT)return 7;
    }
    puts("API32_INSTALLD_CLEAR_PASS app_size=decoded data_removed=1 directories_retained=1");
    if(!systemCall([&](const auto& target){
        for(size_t i=0;i<args.size();i++) {
            const auto& arg=args[i];
            if(!ok(target->destroyAppData(std::nullopt,arg.packageName,0,arg.flags,inodes[i]),"destroyAppData") ||
                !ok(target->destroyAppProfiles(arg.packageName),"destroyAppProfiles"))return false;
        }
        return true;
    }))return 8;
    for(const auto& arg:args)for(bool ce:{false,true}) {
        struct stat st;
        if(lstat(path(arg,ce).c_str(),&st)!=-1 || errno!=ENOENT)return 9;
    }
    puts("API32_INSTALLD_PASS stock_daemon=1 caller_uid=1000 create_size_clear_destroy=1");return 0;
}
