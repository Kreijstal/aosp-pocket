// SPDX-License-Identifier: GPL-3.0-only
#include <sys/system_properties.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc,char** argv) {
    char sdk[PROP_VALUE_MAX];__system_property_get("ro.build.version.sdk",sdk);
    if(argc!=3 || getuid()!=0 || strcmp(sdk,"32"))return 64;
    int result=__system_property_set(argv[1],argv[2]);
    if(result)fprintf(stderr,"POCKET_PROPERTY_WRITE_FAILED name=%s result=%d\n",argv[1],result);
    return result?1:0;
}
