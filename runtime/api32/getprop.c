// SPDX-License-Identifier: GPL-3.0-only
// Property reads use the guest's actual Bionic property area.
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <unistd.h>
static void value(void* unused,const char* name,const char* text,uint32_t serial) {
    (void)unused;(void)serial;printf("[%s]: [%s]\n",name,text);
}
static void entry(const prop_info* info,void* unused) {
    (void)unused;__system_property_read_callback(info,value,NULL);
}
int main(int argc,char** argv) {
    if(argc>3 || syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL)return 64;
    char sdk[PROP_VALUE_MAX];__system_property_get("ro.build.version.sdk",sdk);
    if(strcmp(sdk,"32"))return 64;
    if(argc==1)return __system_property_foreach(entry,NULL)?1:0;
    char text[PROP_VALUE_MAX];int n=__system_property_get(argv[1],text);
    puts(n?text:argc==3?argv[2]:"");return 0;
}
