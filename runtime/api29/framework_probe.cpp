// SPDX-License-Identifier: GPL-3.0-only
#include <binder/IServiceManager.h>
#include <sys/system_properties.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
using namespace android;
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);
    auto manager=defaultServiceManager();
    if(!manager)return 1;
    if(argc==2 && !strcmp(argv[1],"--start")){
        for(unsigned attempt=0;attempt<300;attempt++){
            bool ready=true;
            for(const char* name:{"vold","apexservice","netd","dnsresolver","android.security.keystore","android.service.gatekeeper.IGateKeeperService","SurfaceFlinger"}){
                auto service=manager->checkService(String16(name));
                if(!service || service->pingBinder()!=NO_ERROR){ready=false;break;}
            }
            if(ready){
                if(__system_property_set("pocket.api29.start","1"))return 5;
                puts("API29_PLATFORM_READY services=7 zygote_start=requested");return 0;
            }
            usleep(100000);
        }
        fputs("API29_PLATFORM_READY_TIMEOUT\n",stderr);return 6;
    }
    if(argc!=1)return 64;
    FILE* log=fopen("/tmp/properties.log","r");
    for(unsigned n=0;n<900;n++) {
        if(log){
            clearerr(log);char line[2048];
            while(fgets(line,sizeof(line),log))if(strstr(line,"FATAL EXCEPTION IN SYSTEM PROCESS") ||
                strstr(line,"Abort message:") || strstr(line,"signal 11 (SIGSEGV)")){
                fclose(log);puts("API29_SYSTEM_SERVER_FATAL");return 4;
            }
        }
        char boot[PROP_VALUE_MAX]={};__system_property_get("sys.boot_completed",boot);
        if(!strcmp(boot,"1")) {
            for(const char* name:{"activity","package","power","display","window","input"}) {
                auto service=manager->checkService(String16(name));
                if(!service || service->pingBinder()!=NO_ERROR)return 2;
                printf("API29_FRAMEWORK_SERVICE name=%s ping=passed\n",name);
            }
            puts("API29_FRAMEWORK_SERVICES_PASS boot_completed=1");return 0;
        }
        usleep(100000);
    }
    puts("API29_SYSTEM_SERVER_BOOT_TIMEOUT");return 3;
}
