// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <elf.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <grp.h>
#include "guest_api.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"INIT_LOADER_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
static int security(unsigned expected) {
    CHECK(getuid()==expected);
    FILE* input=fopen("/proc/self/status","re");CHECK(input);
    char line[512];unsigned uid=0;int tracer=-1;unsigned long caps=~0UL;
    while(fgets(line,sizeof(line),input)) {sscanf(line,"Uid: %u",&uid);sscanf(line,"TracerPid: %d",&tracer);sscanf(line,"CapEff: %lx",&caps);}fclose(input);
    CHECK(uid>=10000 && tracer==0 && caps==0);
    input=fopen("/proc/self/attr/current","re");CHECK(input && fgets(line,sizeof(line),input));fclose(input);CHECK(strstr(line,"untrusted_app"));
    input=fopen("/proc/self/maps","re");CHECK(input);
    while(fgets(line,sizeof(line),input))CHECK(!strstr(line,"libproot.so") && !strstr(line,"libpocket_direct_boot.so"));fclose(input);
    Dl_info library;CHECK(dladdr((void*)syscall,&library));
    int fd=open(library.dli_fname,O_RDONLY|O_CLOEXEC);struct stat st;CHECK(fd>=0 && !fstat(fd,&st));
    void* bytes=mmap(NULL,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);CHECK(bytes!=MAP_FAILED);
    Elf64_Ehdr* elf=bytes;CHECK(elf->e_phoff+elf->e_phnum*sizeof(Elf64_Phdr)<=(size_t)st.st_size);
    Elf64_Phdr* headers=(Elf64_Phdr*)((char*)bytes+elf->e_phoff);unsigned compared=0;
    for(unsigned i=0;i<elf->e_phnum;i++)if(headers[i].p_type==PT_LOAD && (headers[i].p_flags&PF_X)) {
        CHECK(headers[i].p_offset+headers[i].p_filesz<=(size_t)st.st_size);
        CHECK(!memcmp((char*)library.dli_fbase+headers[i].p_vaddr,(char*)bytes+headers[i].p_offset,headers[i].p_filesz));compared+=headers[i].p_filesz;
    }
    CHECK(compared>0);munmap(bytes,st.st_size);close(fd);
    printf("NATIVE_SECURITY_PASS guestUid=%u kernelUid=%u tracerPid=0 caps=0 libcBytes=%u differentBytes=0\n",expected,uid,compared);return 0;
}
int main(int argc,char** argv) {
    setbuf(stdout,NULL);setbuf(stderr,NULL);
    if(argc==2) {
        CHECK(!security(1000));CHECK(!*environ);gid_t groups[2];
        CHECK(getgroups(2,groups)==2 && groups[0]==1000 && groups[1]==3010);
        CHECK(setuid(0)==-1 && errno==EPERM);
        CHECK(pocket_native_setgroups(0,NULL)==-1 && errno==EPERM);
        puts("NATIVE_MANAGER_CHILD_PASS uid=1000 groups=1000,3010 empty_environment=1 regain_root_denied=1");return 0;
    }
    CHECK(argc==1 && !security(0));gid_t groups[]={1000,3010};
    CHECK(!pocket_native_setgroups(2,groups));CHECK(getgroups(0,NULL)==2);
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(!setresgid(1000,1000,1000) && !setresuid(1000,1000,1000));
        char* arguments[]={argv[0],"child",NULL};char* empty[]={NULL};
        execve(argv[0],arguments,empty);perror("native exec");_exit(127);
    }
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    puts("NATIVE_MANAGER_RUNTIME_PASS kernel_credentials=unchanged bionic_patches=0 proot=0 groups_across_exec=1");return 0;
}
