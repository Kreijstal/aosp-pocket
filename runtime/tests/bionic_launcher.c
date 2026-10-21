// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Android's ARM64 static Bionic startup requires a 64-byte TLS alignment.
static _Thread_local volatile char launcher_tls[64] __attribute__((aligned(64)));

// Static launcher: exec the test with a private shim, without the running
// guest's dynamic exec wrapper replacing our test environment/preload list.
int main(int argc,char** argv) {
    launcher_tls[0]=1;
    if(argc!=5)return 2;
    char *root=NULL,*libraries=NULL,*preload=NULL,*mode=NULL,*config=NULL,*fixture=NULL;
    if(asprintf(&root,"POCKET_DIRECT_ROOT=%s",argv[1])<0 ||
       asprintf(&libraries,"LD_LIBRARY_PATH=%s/system/lib64",argv[1])<0 ||
       asprintf(&preload,"LD_PRELOAD=%s/libpocket_direct_boot.so",argv[2])<0 ||
       asprintf(&mode,"POCKET_BIONIC_MODE=%s",argv[3])<0 ||
       asprintf(&config,"LD_CONFIG_FILE=%s/system/etc/ld.config.txt",argv[1])<0 ||
       asprintf(&fixture,"%s/pocket-bionic-probe/fixture.txt",argv[1])<0)return 3;
    char* env[]={root,libraries,preload,mode,config,"POCKET_DIRECT_ID=12345:12345",NULL};
    char* args[]={argv[4],argv[3],"/pocket-bionic-probe/fixture.txt",fixture,NULL};
    execve(argv[4],args,env);
    perror("bionic probe exec");return 4;
}
