// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

// Compare the entire loaded executable segments, not just selected symbols.
int main(int argc,char** argv) {
    if(argc!=4)return 2;
    int unmodified=!strcmp(argv[1],"unmodified");
    Dl_info library;
    if(!dladdr((void*)syscall,&library) || !library.dli_fname)return 3;
    int fd=syscall(SYS_openat,AT_FDCWD,library.dli_fname,O_RDONLY|O_CLOEXEC,0);
    struct stat st;
    if(fd<0 || syscall(SYS_fstat,fd,&st))return 4;
    unsigned char* disk=mmap(NULL,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    if(disk==MAP_FAILED)return 5;
    Elf64_Ehdr* eh=(Elf64_Ehdr*)disk;
    if(memcmp(eh->e_ident,ELFMAG,SELFMAG) || eh->e_phentsize!=sizeof(Elf64_Phdr) ||
        eh->e_phoff+(size_t)eh->e_phnum*sizeof(Elf64_Phdr)>(size_t)st.st_size)return 6;
    Elf64_Phdr* ph=(Elf64_Phdr*)(disk+eh->e_phoff);
    size_t compared=0,different=0;
    for(unsigned i=0;i<eh->e_phnum;++i) {
        if(ph[i].p_type!=PT_LOAD || !(ph[i].p_flags&PF_X))continue;
        if(ph[i].p_offset+ph[i].p_filesz>(size_t)st.st_size)return 7;
        const unsigned char* loaded=(unsigned char*)library.dli_fbase+ph[i].p_vaddr;
        for(size_t j=0;j<ph[i].p_filesz;++j)different+=loaded[j]!=disk[ph[i].p_offset+j];
        compared+=ph[i].p_filesz;
    }
    munmap(disk,st.st_size);close(fd);
    unsigned kernel_uid=syscall(SYS_getuid),libc_uid=getuid();
    int virtual_fd=openat(AT_FDCWD,argv[2],O_RDONLY|O_CLOEXEC);
    int virtual_errno=virtual_fd<0?errno:0;
    if(virtual_fd>=0)close(virtual_fd);
    int backing_fd=openat(AT_FDCWD,argv[3],O_RDONLY|O_CLOEXEC);
    char content[32]={0};
    int backing_ok=backing_fd>=0 && read(backing_fd,content,sizeof(content)-1)>0 && !strcmp(content,"bionic-mode-fixture\n");
    if(backing_fd>=0)close(backing_fd);
    printf("{\"mode\":\"%s\",\"executableBytesCompared\":%zu,\"differentBytes\":%zu,"
        "\"libcUid\":%u,\"kernelUid\":%u,\"virtualPathErrno\":%d,\"backingPathReadable\":%s}\n",
        argv[1],compared,different,libc_uid,kernel_uid,virtual_errno,backing_ok?"true":"false");
    if(!compared || !backing_ok)return 8;
    if(unmodified)return different || libc_uid!=kernel_uid || virtual_errno!=ENOENT;
    return !different || libc_uid!=12345 || virtual_errno!=0;
}
