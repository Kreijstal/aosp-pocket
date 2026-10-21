// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do {if(!(x)){fprintf(stderr,"LOADER_PROBE_FAIL line=%d errno=%d\n",__LINE__,errno);exit(1);}}while(0)
static unsigned compared;
static void bytes_unchanged(void) {
    Dl_info library;CHECK(dladdr((void*)syscall,&library) && library.dli_fname);
    int fd=open(library.dli_fname,O_RDONLY|O_CLOEXEC);struct stat st;CHECK(fd>=0 && !fstat(fd,&st));
    unsigned char* disk=mmap(NULL,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);CHECK(disk!=MAP_FAILED);
    Elf64_Ehdr* eh=(Elf64_Ehdr*)disk;
    CHECK(eh->e_phoff+(size_t)eh->e_phnum*sizeof(Elf64_Phdr)<=(size_t)st.st_size);
    Elf64_Phdr* ph=(Elf64_Phdr*)(disk+eh->e_phoff);compared=0;
    for(unsigned i=0;i<eh->e_phnum;i++)if(ph[i].p_type==PT_LOAD && (ph[i].p_flags&PF_X)) {
        CHECK(ph[i].p_offset+ph[i].p_filesz<=(size_t)st.st_size);
        CHECK(!memcmp((char*)library.dli_fbase+ph[i].p_vaddr,disk+ph[i].p_offset,ph[i].p_filesz));
        compared+=ph[i].p_filesz;
    }
    CHECK(compared>0);munmap(disk,st.st_size);close(fd);
}
static long direct_open(const char* path) {
    register long x0 __asm__("x0")=AT_FDCWD,x1 __asm__("x1")=(long)path,x2 __asm__("x2")=O_RDONLY,x3 __asm__("x3")=0;
    register long x8 __asm__("x8")=__NR_openat;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x8):"memory","cc");return x0;
}
static void contents(int fd) {
    char value[32]={0};CHECK(fd>=0 && read(fd,value,sizeof(value))==15 && !memcmp(value,"loader-fixture\n",15));close(fd);
}
static void* thread(void* arg) {(void)arg;for(unsigned i=0;i<12;i++)contents(direct_open("/fixture.txt"));return NULL;}
static unsigned status(void) {
    FILE* file=fopen("/proc/self/status","re");CHECK(file);char line[512];int tracer=-1,seccomp=-1,filters=-1;unsigned long caps=~0UL;
    while(fgets(line,sizeof(line),file)) {
        sscanf(line,"TracerPid: %d",&tracer);sscanf(line,"Seccomp: %d",&seccomp);
        sscanf(line,"Seccomp_filters: %d",&filters);sscanf(line,"CapEff: %lx",&caps);
    }
    fclose(file);CHECK(tracer==0 && seccomp==2 && filters>0 && caps==0);
    CHECK(getuid()>=10000 && getuid()==(uid_t)syscall(SYS_getuid));
    file=fopen("/proc/self/attr/current","re");CHECK(file && fgets(line,sizeof(line),file));fclose(file);
    CHECK(strstr(line,"untrusted_app"));
    file=fopen("/proc/self/maps","re");CHECK(file);
    while(fgets(line,sizeof(line),file))CHECK(!strstr(line,"libproot.so") && !strstr(line,"libpocket_direct_boot.so"));
    fclose(file);return (unsigned)filters;
}
static void dummy(int signal) {(void)signal;}
int main(int argc,char** argv) {
    setbuf(stdout,NULL);setbuf(stderr,NULL);
    unsigned filters=status();bytes_unchanged();contents(open("/fixture.txt",O_RDONLY));contents(direct_open("/fixture.txt"));
    int directory=open("/",O_RDONLY|O_DIRECTORY);CHECK(directory>=0);contents(openat(directory,"fixture.txt",O_RDONLY));close(directory);
    struct stat st;CHECK(!fstatat(AT_FDCWD,"/fixture.txt",&st,0) && st.st_size==15);
    CHECK(!faccessat(AT_FDCWD,"/fixture.txt",R_OK,0));
    char link[64]={0};CHECK(readlink("/fixture-link",link,sizeof(link))==11 && !memcmp(link,"fixture.txt",11));
    CHECK(readlink("/fixture-link",link,4)==4 && !memcmp(link,"fixt",4));
    CHECK(syscall(SYS_readlinkat,AT_FDCWD,"/fixture-link",(void*)1,64)==-1 && errno==EFAULT);
    static const char read_only[]="read-only-buffer";
    CHECK(syscall(SYS_readlinkat,AT_FDCWD,"/fixture-link",(void*)read_only,sizeof(read_only))==-1 && errno==EFAULT);
    CHECK(!strcmp(read_only,"read-only-buffer"));
    memset(link,0,sizeof(link));CHECK(readlink("/proc/self/exe",link,sizeof(link))==17 && !memcmp(link,"/system/bin/probe",17));
    CHECK(direct_open((const char*)1)==-EFAULT);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    char* guarded=mmap(NULL,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(guarded!=MAP_FAILED);
    const char* fixture="/fixture.txt";size_t path_size=strlen(fixture)+1;
    memcpy(guarded+page-path_size,fixture,path_size);memcpy(guarded+page,fixture,path_size);
    CHECK(!mprotect(guarded+page,page,PROT_NONE));
    contents(direct_open(guarded+page-path_size));
    CHECK(direct_open(guarded+page)==-EFAULT);CHECK(!munmap(guarded,2*page));
    char* empty[]={NULL};char* bad[]={"bad",NULL};
    CHECK(execve("/missing",bad,empty)==-1 && errno==ENOENT);
    CHECK(execve("/system/bin/unprepared",bad,empty)==-1 && errno==ENOEXEC);
    CHECK(execve("/script.sh",bad,empty)==-1 && errno==ENOEXEC);
    struct sigaction action={0};action.sa_handler=dummy;
    CHECK(sigaction(SIGSYS,&action,NULL)==-1 && errno==EBUSY);
    pthread_t workers[2];for(unsigned i=0;i<2;i++)CHECK(!pthread_create(&workers[i],NULL,thread,NULL));
    for(unsigned i=0;i<2;i++)CHECK(!pthread_join(workers[i],NULL));
    if(argc==4) {
        unsigned stage=(unsigned)atoi(argv[2]),expected=(unsigned)atoi(argv[3]);
        CHECK(!strcmp(argv[0],"preserved argv[0]") && !strcmp(argv[1],"chain"));
        CHECK(filters==expected && stage>=1 && stage<=16);
        const char* environment=getenv("POCKET_EXEC_FIXTURE");
        CHECK(stage%2 ? environment && !strcmp(environment,"preserved") : !environment && !*environ);
        sigset_t mask;CHECK(!sigprocmask(SIG_SETMASK,NULL,&mask));
        CHECK(sigismember(&mask,SIGUSR1)==1 && sigismember(&mask,SIGSYS)==0);
        if(stage==16) {
            printf("LOADER_CHAIN_PASS execs=%u filters=%u libcBytes=%u differentBytes=0 tracerPid=0 kernelUid=%u\n",stage,filters,compared,getuid());return 0;
        }
        char next[16];snprintf(next,sizeof(next),"%u",stage+1);
        char* args[]={"preserved argv[0]","chain",next,argv[3],NULL};
        char* env[]={"POCKET_EXEC_FIXTURE=preserved",NULL};
        execve("/system/bin/probe",args,(stage+1)%2?env:empty);CHECK(0);
    }
    CHECK(argc==1);
    // Exercise real prepared Android 9 shell and toybox executables too.
    pid_t shell=fork();CHECK(shell>=0);
    if(!shell) {
        char* args[]={"sh","-c","/system/bin/toybox cat /fixture.txt && [ -r /fixture.txt ] && echo SHELL_EXEC_PASS",NULL};
        execve("/system/bin/sh",args,empty);CHECK(0);
    }
    int shell_status;CHECK(waitpid(shell,&shell_status,0)==shell && WIFEXITED(shell_status) && WEXITSTATUS(shell_status)==0);
    sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);CHECK(!sigprocmask(SIG_BLOCK,&mask,NULL));
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        char count[16];snprintf(count,sizeof(count),"%u",filters);
        char* args[]={"preserved argv[0]","chain","1",count,NULL};
        char* env[]={"POCKET_EXEC_FIXTURE=preserved",NULL};
        execve("/system/bin/probe",args,env);CHECK(0);
    }
    int child_status;CHECK(waitpid(child,&child_status,0)==child && WIFEXITED(child_status) && WEXITSTATUS(child_status)==0);
    CHECK(status()==filters);puts("LOADER_TEST_PASS fork=1 threads=1 exec=16 empty_env=1 argv=1 mask=1 unchanged_bionic=1 no_tracer=1");return 0;
}
