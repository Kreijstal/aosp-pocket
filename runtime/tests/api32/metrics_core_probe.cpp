// SPDX-License-Identifier: GPL-3.0-only
#include <android/media/IMediaMetricsService.h>
#include <binder/IServiceManager.h>
#include <media/MediaMetricsItem.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>

using namespace android;
int main(int argc,char** argv) {
    if(argc!=3 || getuid()!=0 || syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL)return 64;
    const std::string nonce=argv[2];
    if(nonce.size()!=32 || nonce.find_first_not_of("0123456789abcdef")!=std::string::npos)return 64;
    const bool submit=!strcmp(argv[1],"submit"),verify=!strcmp(argv[1],"verify");
    if(!submit && !verify)return 64;
    if(submit && (setgid(10021) || setuid(10021)))return 1;
    auto manager=defaultServiceManager();if(!manager)return 2;
    auto binder=manager->getService(String16("media.metrics"));if(!binder || binder->pingBinder()!=NO_ERROR)return 3;
    if(submit) {
        auto service=interface_cast<media::IMediaMetricsService>(binder);if(!service)return 4;
        // AIDL oneway calls to one Binder are ordered. Observe the allowed
        // record only after the preceding forbidden submission was processed.
        for(const char* key:{"pocket.forbidden","extractor"}) {
            mediametrics::Item item(key);item.setUid(1000).setPid(7).setPkgName("android");
            const std::string tag=nonce+(key[0]=='e'?"-allowed":"-denied");item.setCString("pocket.core.probe",tag.c_str());
            char* bytes=nullptr;size_t length=0;
            if(item.writeToByteString(&bytes,&length)!=NO_ERROR || !bytes)return 5;
            std::vector<uint8_t> payload(bytes,bytes+length);free(bytes);
            if(!service->submitBuffer(payload).isOk())return 6;
        }
        puts("API32_CORE_METRICS_SUBMITTED uid=10021 claimed_uid=1000 oneway=1");return 0;
    }
    std::string path="/tmp/pocket-metrics-dump-"+nonce;
    int fd=open(path.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(fd<0)return 7;
    unlink(path.c_str());
    Vector<String16> args;args.add(String16("--all"));bool found=false;
    for(unsigned attempt=0;attempt<20;attempt++) {
        if(ftruncate(fd,0) || lseek(fd,0,SEEK_SET)<0 || binder->dump(fd,args)!=NO_ERROR){close(fd);return 8;}
        if(lseek(fd,0,SEEK_SET)<0){close(fd);return 9;}
        std::string dump;char buffer[4096];ssize_t count;
        while((count=read(fd,buffer,sizeof(buffer)))>0){dump.append(buffer,count);if(dump.size()>1024*1024){close(fd);return 10;}}
        if(count<0){close(fd);return 11;}
        if(dump.find(nonce+"-denied")!=std::string::npos){close(fd);return 12;}
        size_t at=dump.find(nonce+"-allowed");
        if(at!=std::string::npos) {
            size_t begin=dump.rfind('\n',at),end=dump.find('\n',at);
            std::string line=dump.substr(begin==std::string::npos?0:begin+1,end==std::string::npos?std::string::npos:end-(begin==std::string::npos?0:begin+1));
            if(line.find(", 10021)")==std::string::npos || line.find("(android,")!=std::string::npos){close(fd);return 13;}
            found=true;break;
        }
        usleep(250000);
    }
    close(fd);if(!found)return 14;
    puts("API32_CORE_METRICS_VERIFIED stock_record=1 caller_uid=10021 forged_identity_replaced=1 forbidden_key_absent=1");return 0;
}
