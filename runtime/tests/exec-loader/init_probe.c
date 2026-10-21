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
static int transfer(int fd,void* bytes,size_t size,int write_out) {
    char* p=bytes;while(size) {ssize_t n=write_out?write(fd,p,size):read(fd,p,size);
        if(n<0 && errno==EINTR)continue;if(n<=0)return 0;p+=n;size-=(size_t)n;}return 1;
}
static int client(void) {
    CHECK(!security(39999));CHECK(getuid()==39999 && getgid()==39999);
    CHECK(!*environ); // The exec transport is consumed before stock Bionic.
    struct stat owner;CHECK(!stat("/data/fs-owner",&owner) && owner.st_uid==39999 && owner.st_gid==39999);
    CHECK(setuid(0)==-1 && errno==EPERM);
    int fd=-1;struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,"/dev/socket/zygote");
    for(unsigned i=0;i<300;i++) {fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);CHECK(fd>=0);
        if(!connect(fd,(struct sockaddr*)&address,sizeof(address)))break;
        close(fd);fd=-1;usleep(200000);}
    CHECK(fd>=0);struct timeval timeout={30,0};CHECK(!setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
    char query[]="1\n--query-abi-list\n";CHECK(transfer(fd,query,strlen(query),1));
    unsigned size;CHECK(transfer(fd,&size,sizeof(size),0));size=ntohl(size);CHECK(size==9);
    char abi[10]={0};CHECK(transfer(fd,abi,size,0) && !strcmp(abi,"arm64-v8a"));
    puts("NATIVE_ZYGOTE_ABI_PASS arm64-v8a");
    char request[]="8\n--runtime-args\n--setuid=39999\n--setgid=39999\n--runtime-flags=0\n--target-sdk-version=28\n--nice-name=loader-java-child\n--instruction-set=arm64\ndev.kreijstal.aosppocket.guest.LoaderChild\n";
    CHECK(transfer(fd,request,strlen(request),1));unsigned pid;unsigned char wrapper;
    CHECK(transfer(fd,&pid,sizeof(pid),0) && transfer(fd,&wrapper,1,0));pid=ntohl(pid);
    CHECK((int)pid>1 && !wrapper);printf("NATIVE_ZYGOTE_FORK pid=%u\n",pid);
    int found=0;for(unsigned i=0;i<400;i++) {int report=open("/data/loader-child.txt",O_RDONLY|O_CLOEXEC);
        if(report>=0) {char value[256]={0};ssize_t n=read(report,value,sizeof(value)-1);close(report);
            if(n>0 && strstr(value,"NATIVE_JAVA_CHILD_PASS uid=39999 gid=39999 threads=1")) {fputs(value,stdout);found=1;break;}}
        usleep(100000);}
    CHECK(found);
    CHECK(transfer(fd,query,strlen(query),1) && transfer(fd,&size,sizeof(size),0) && ntohl(size)==9 && transfer(fd,abi,9,0));
    close(fd);puts("NATIVE_ZYGOTE_REUSE_PASS");return 0;
}
static int filesystem(void) {
    CHECK(getuid()==0 && geteuid()==0 && getgid()==0);
    char cwd[4096];CHECK(getcwd(cwd,sizeof(cwd)) && !strcmp(cwd,"/"));
    CHECK(!mkdir("/data/fs-check",0700) || errno==EEXIST);
    CHECK(!chdir("/data/fs-check"));CHECK(getcwd(cwd,sizeof(cwd)) && !strcmp(cwd,"/data/fs-check"));
    int fd=open("item",O_CREAT|O_RDWR|O_TRUNC,0600);CHECK(fd>=0 && write(fd,"native",6)==6);
    CHECK(!fchown(fd,39999,39999));struct stat st;CHECK(!fstat(fd,&st) && st.st_uid==39999 && st.st_gid==39999);close(fd);
    CHECK(!rename("item","renamed"));CHECK(!stat("renamed",&st) && st.st_uid==39999);
    unlink("absolute");CHECK(!symlink("/data/fs-check/renamed","absolute"));
    fd=open("absolute",O_RDONLY);CHECK(fd>=0);close(fd);
    memset(cwd,0,sizeof(cwd));CHECK(readlink("absolute",cwd,sizeof(cwd)-1)>0 && !strcmp(cwd,"/data/fs-check/renamed"));
    CHECK(!unlink("absolute") && !unlink("renamed") && !chdir("/"));
    fd=open("/data/fs-owner",O_CREAT|O_RDWR|O_TRUNC,0600);CHECK(fd>=0 && !fchown(fd,39999,39999));close(fd);
    int socket_fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);CHECK(socket_fd>=0);
    struct sockaddr_un a={.sun_family=AF_UNIX};strcpy(a.sun_path,"/dev/socket/fs-check");unlink(a.sun_path);
    CHECK(!bind(socket_fd,(struct sockaddr*)&a,sizeof(a)));socklen_t count=sizeof(a);
    CHECK(!getsockname(socket_fd,(struct sockaddr*)&a,&count) && !strcmp(a.sun_path,"/dev/socket/fs-check"));
    close(socket_fd);CHECK(!unlink(a.sun_path));puts("NATIVE_FILESYSTEM_PASS cwd=1 rename=1 symlink=1 ownership=1 socket=1");return 0;
}
int main(int argc,char** argv) {
    (void)argv;setbuf(stdout,NULL);setbuf(stderr,NULL);if(argc==2)return client();
    CHECK(!security(0));CHECK(!filesystem());unlink("/data/loader-child.txt");
    pid_t init=fork();CHECK(init>=0);
    if(!init) {execl("/system/bin/pocket-init-api28","pocket-init-api28","/system/etc/init/init.native.rc",NULL);perror("exec init");_exit(126);}
    pid_t tester=fork();if(tester<0) {kill(init,SIGTERM);waitpid(init,NULL,0);return 1;}
    if(!tester) {if(setgid(39999) || setuid(39999))_exit(127);
        char* arguments[]={"init-probe","client",NULL};char* empty[]={NULL};
        execve("/system/bin/init-probe",arguments,empty);perror("exec client");_exit(126);}
    int status=0;waitpid(tester,&status,0);kill(init,SIGTERM);int init_status=0;waitpid(init,&init_status,0);
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status));CHECK(WIFEXITED(init_status) && !WEXITSTATUS(init_status));
    CHECK(access("/dev/socket/zygote",F_OK)==-1 && errno==ENOENT);
    CHECK(access("/dev/socket/property_service",F_OK)==-1 && errno==ENOENT);
    puts("NATIVE_INIT_ZYGOTE_PASS init=android9 zygote=android9 fork=1 guest_uid=39999 proot=0 bionic_patches=0");return 0;
}
