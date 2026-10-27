// SPDX-License-Identifier: GPL-3.0-only
// /system/bin/app_process64 for programs that start Java code by the stock
// name, such as libsu's root service. Zygote does not pass Pocket's Binder and
// HIDL shims on to programs its children exec; the Java runtime needs them.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SHIMS "/system/lib64/libpocket_binder_api28.so:/system/lib64/libpocket_hidl_api28.so"

int main(int argc,char** argv) {
    (void)argc;
    const char* current=getenv("LD_PRELOAD");
    if(!current || !strstr(current,"libpocket_binder_api28.so")) {
        char* value=SHIMS;
        if(current && *current && asprintf(&value,"%s:%s",SHIMS,current)<0)return 127;
        if(setenv("LD_PRELOAD",value,1))return 127;
    }
    execv("/system/bin/pocket-app-process-api28",argv);
    perror("pocket-app-process-api28");
    return 127;
}
