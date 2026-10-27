// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"DENY_FAIL line=%d errno=%d\n",__LINE__,errno);exit(1);}}while(0)
static void content(const char* path,const char* expected) {
 int fd=open(path,O_RDONLY);CHECK(fd>=0);char b[64]={0};CHECK(read(fd,b,sizeof(b)-1)>0);close(fd);CHECK(!strcmp(b,expected));
}
static void absent(const char* path) {
 struct stat st;errno=0;CHECK(stat(path,&st)<0 && errno==ENOENT);errno=0;CHECK(access(path,F_OK)<0 && errno==ENOENT);errno=0;CHECK(open(path,O_RDONLY)<0 && errno==ENOENT);
}
static void entries(const char* path,const char* hidden) {
 DIR* d=opendir(path);CHECK(d);struct dirent* e;while((e=readdir(d)))CHECK(strcmp(e->d_name,hidden));closedir(d);
}
static void verify(const char* root) {
 CHECK(syscall(SYS_getpid,0x504c2001UL,4UL)==1);content("/system/etc/denylist-view","base");
 absent("/system/bin/su");absent("/system//bin/./su");absent("/system/etc/../bin/su");absent("/sbin/.magisk");absent("/data/adb");absent("/.pocket-module-view/system/etc/denylist-view");absent("/alias-su");
 char path[2048];snprintf(path,sizeof(path),"%s/system/bin/su",root);absent(path);
 int d=open("/system/bin",O_RDONLY|O_DIRECTORY);CHECK(d>=0);CHECK(openat(d,"su",O_RDONLY)<0 && errno==ENOENT);
 snprintf(path,sizeof(path),"/proc/self/fd/%d/su",d);absent(path);close(d);
 CHECK(chdir("/system")==0);absent("bin/su");CHECK(chdir("/")==0);
 entries("/",".pocket-module-view");entries("/data","adb");entries("/system/bin","su");
 int data=open("/system/etc/denylist-view",O_RDONLY);CHECK(data>=0);void* image=mmap(NULL,4096,PROT_READ,MAP_PRIVATE,data,0);CHECK(image!=MAP_FAILED);close(data);
 int fd=open("/proc/self/maps",O_RDONLY);CHECK(fd>=0);char* maps=calloc(1,1024*1024);CHECK(maps);size_t n=0;ssize_t got;
 while((got=read(fd,maps+n,1024*1024-1-n))>0)n+=got;close(fd);CHECK(!strstr(maps,"/.pocket-module-view/"));CHECK(strstr(maps,"/system/etc/denylist-view"));free(maps);munmap(image,4096);
}
int main(int argc,char** argv) {
 setbuf(stdout,NULL);CHECK(argc>=2);
 if(argc==3){verify(argv[1]);CHECK(syscall(SYS_getpid,0x504c2001UL,3UL)<0 && errno==EPERM);puts("DENYLIST_EXEC_PASS");return 0;}
 CHECK(syscall(SYS_getpid,0x504c2001UL,4UL)==0);content("/system/etc/denylist-view","overlay");CHECK(access("/system/bin/su",F_OK)==0);
 int image=open("/system/etc/denylist-view",O_RDONLY);CHECK(image>=0);void* mapped=mmap(NULL,4096,PROT_READ,MAP_PRIVATE,image,0);CHECK(mapped!=MAP_FAILED);close(image);
 int inherited=open("/system/bin",O_RDONLY|O_DIRECTORY);CHECK(inherited>=0);
 pid_t child=fork();CHECK(child>=0);
 if(!child) {
  CHECK(syscall(SYS_getpid,0x504c2001UL,3UL)==0);CHECK(setgid(19991)==0 && setuid(19991)==0);
  CHECK(openat(inherited,"su",O_RDONLY)<0 && errno==ENOENT);
  char p[128];snprintf(p,sizeof(p),"/proc/self/fd/%d/su",inherited);absent(p);close(inherited);verify(argv[1]);
  char* args[]={"/system/bin/denylist-probe",argv[1],"verify",NULL};char* env[]={NULL};execve(args[0],args,env);CHECK(0);
 }
 close(inherited);int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status)&&WEXITSTATUS(status)==0);
 CHECK(syscall(SYS_getpid,0x504c2001UL,4UL)==0);content("/system/etc/denylist-view","overlay");CHECK(access("/system/bin/su",F_OK)==0);munmap(mapped,4096);
 puts("DENYLIST_VIEW_PASS base=1 paths=1 relative=1 inherited_dirfd=1 proc_alias=1 symlink=1 enumeration=1 fork=1 exec=1 maps=1 unlisted_unchanged=1");return 0;
}
