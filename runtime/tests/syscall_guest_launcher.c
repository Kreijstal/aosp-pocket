// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Thread_local volatile char launcher_tls[64] __attribute__((aligned(64)));

// Diagnostic Android 9 launcher with no Bionic shim/preload. The tracer owns
// pathname and identity translation across exec, threads and child processes.
int main(int argc,char** argv) {
    launcher_tls[0]=1;
    if(argc<4)return 2; // private root, tracer dir, guest command...
    unsigned first=3;
    char* console=NULL;
    if(argc>5 && !strcmp(argv[3],"--log")) {
        size_t prefix=strlen(argv[1]);
        if(strncmp(argv[4],argv[1],prefix) || argv[4][prefix]!='/')return 2;
        int log=open(argv[4],O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
        if(log<0 || dup2(log,STDOUT_FILENO)<0 || dup2(log,STDERR_FILENO)<0)return 3;
        close(log);
        int null=open("/dev/null",O_RDONLY|O_CLOEXEC);
        if(null<0 || dup2(null,STDIN_FILENO)<0)return 3;
        close(null);
        if(asprintf(&console,"%s:/pocket-console",argv[4])<0)return 3;
        first=5;
    }
    char *proot=NULL,*tmp=NULL,*sockets=NULL,*properties=NULL,*input=NULL,*data=NULL;
    if(asprintf(&proot,"%s/libproot.so",argv[2])<0 ||
       asprintf(&tmp,"PROOT_TMP_DIR=%s",argv[2])<0 ||
       asprintf(&sockets,"%s/dev/socket:/dev/socket",argv[1])<0 ||
       asprintf(&properties,"%s/dev/__properties__:/dev/__properties__",argv[1])<0 ||
       asprintf(&input,"%s/dev/input:/dev/input",argv[1])<0 ||
       asprintf(&data,"%s/system-data:/data",argv[1])<0)return 3;
    char* env[]={"PROOT_NO_SECCOMP=1","POCKET_PROOT_CRASH_DIAGNOSTICS=1","POCKET_PROOT_OWNER=1","POCKET_SYSCALL_BACKEND=proot",tmp,
        "PATH=/system/bin","ANDROID_ROOT=/system","ANDROID_DATA=/data","ANDROID_ASSETS=/system/app",
        "ANDROID_STORAGE=/storage","ASEC_MOUNTPOINT=/mnt/asec","EXTERNAL_STORAGE=/sdcard","TMPDIR=/tmp",
        "POCKET_GUEST_CONSOLE=/logs/framework-test.log",NULL};
    DIR* descriptors=opendir("/proc/self/fd");
    if(!descriptors)return 3;
    struct dirent* entry;
    while((entry=readdir(descriptors))) {
        char* end;long fd=strtol(entry->d_name,&end,10);
        if(*entry->d_name && !*end && fd>2 && fd!=dirfd(descriptors))close((int)fd);
    }
    closedir(descriptors);
    char** args=calloc((size_t)argc+26,sizeof(char*));
    if(!args)return 3;
    unsigned n=0;
    args[n++]=proot;args[n++]="--kill-on-exit";args[n++]="-r";args[n++]=argv[1];
    args[n++]="-0";args[n++]="-w";args[n++]="/";
    args[n++]="-b";args[n++]="/dev";args[n++]="-b";args[n++]="/proc";
    args[n++]="-b";args[n++]="/sys";
    // Match the existing virtual HIDL transport using the app's ordinary
    // Binder driver, without redirecting Bionic openat().
    args[n++]="-b";args[n++]="/dev/binder:/dev/hwbinder";
    args[n++]="-b";args[n++]=sockets;args[n++]="-b";args[n++]=properties;
    args[n++]="-b";args[n++]=input;
    args[n++]="-b";args[n++]=data;
    if(console) {args[n++]="-b";args[n++]=console;}
    for(unsigned i=first;i<(unsigned)argc;++i)args[n++]=argv[i];
    execve(proot,args,env);
    perror("syscall guest exec");return 4;
}
