// SPDX-License-Identifier: GPL-3.0-only
#include <dlfcn.h>
#include <stdio.h>
int main(void) {
    int (*start)(void)=dlsym(RTLD_DEFAULT,"pocketStartHardwareServices");
    if(!start){fputs("DIRECT_HWB_RUNTIME_MISSING\n",stderr);return 1;}
    return start();
}
