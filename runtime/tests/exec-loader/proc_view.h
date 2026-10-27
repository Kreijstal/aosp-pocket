// SPDX-License-Identifier: GPL-3.0-only
// Exercise the real proc interface, including inline SVC and inherited fds.
#include <sys/uio.h>

static char* proc_read_all(int fd) {
    size_t used=0,capacity=4*1024*1024;char* text=malloc(capacity+1);CHECK(text);
    for(;;) {
        CHECK(used<capacity);ssize_t n=read(fd,text+used,capacity-used);CHECK(n>=0);
        if(!n)break;used+=(size_t)n;
    }
    text[used]=0;return text;
}
static void proc_report(int fd) {
    if(fd<0)fprintf(stderr,"PROC_VIEW_OPEN_FAILED fd=%d\n",fd);
    CHECK(fd>=0);
    struct stat st,linked;CHECK(!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size==0 && st.st_nlink==1);
    char descriptor[64],target[256]={0};snprintf(descriptor,sizeof(descriptor),"/proc/self/fd/%d",fd);
    ssize_t n=readlink(descriptor,target,sizeof(target)-1);CHECK(n>0);target[n]=0;
    CHECK(strstr(target,"/proc/")==target && strstr(target,"/maps"));
    CHECK(!stat(descriptor,&linked) && linked.st_dev==st.st_dev && linked.st_ino==st.st_ino && linked.st_size==0);
    CHECK(!fstatat(fd,"",&linked,AT_EMPTY_PATH) && linked.st_ino==st.st_ino);
    int pathfd=open(descriptor,O_PATH|O_CLOEXEC);CHECK(pathfd>=0 && !fstat(pathfd,&linked) && linked.st_ino==st.st_ino);close(pathfd);
    CHECK(mmap(NULL,4096,PROT_READ,MAP_PRIVATE,fd,0)==MAP_FAILED && errno==ENODEV);
    CHECK(write(fd,"x",1)==-1 && errno==EBADF);
    CHECK(pwrite(fd,"x",1,0)==-1 && errno==EBADF);
    CHECK(ftruncate(fd,0)==-1 && errno==EBADF);
    CHECK((fcntl(fd,F_GETFL)&O_ACCMODE)==O_RDONLY);
    CHECK(lseek(fd,0,SEEK_SET)==0);
    char* text=proc_read_all(fd);
    CHECK(strstr(text,"/system/bin/pocket-loader") && strstr(text,"/system/bin/linker64") && strstr(text,"/system/lib64/libc.so"));
    CHECK(!strstr(text,"/files/root/"));
    CHECK(lseek(fd,0,SEEK_SET)==0);
    char first[19],second[17];struct iovec parts[]={{first,sizeof(first)},{second,sizeof(second)}};
    CHECK(readv(fd,parts,2)==(ssize_t)(sizeof(first)+sizeof(second)));
    CHECK(!memcmp(first,text,sizeof(first)) && !memcmp(second,text+sizeof(first),sizeof(second)));
    CHECK(pread(fd,first,sizeof(first),0)==sizeof(first) && !memcmp(first,text,sizeof(first)));
    CHECK(lseek(fd,0,SEEK_CUR)==(off_t)(sizeof(first)+sizeof(second)));
    int alias=dup(fd);CHECK(alias>=0 && lseek(alias,0,SEEK_SET)==0 && lseek(fd,0,SEEK_CUR)==0);close(alias);
    int reopened=open(descriptor,O_RDONLY|O_CLOEXEC);CHECK(reopened>=0);
    CHECK(read(reopened,first,1)==1 && first[0]==text[0] && lseek(fd,0,SEEK_CUR)==0);close(reopened);
    snprintf(descriptor,sizeof(descriptor),"/proc/self/fdinfo/%d",fd);
    int info=open(descriptor,O_RDONLY);CHECK(info>=0);char* description=proc_read_all(info);close(info);
    unsigned long inode=0,flags=~0UL;long position=-1;int mount=0;
    for(char* p=description;*p;) {
        sscanf(p,"ino: %lu",&inode);sscanf(p,"flags: %lo",&flags);
        sscanf(p,"pos: %ld",&position);sscanf(p,"mnt_id: %d",&mount);
        char* next=strchr(p,'\n');if(!next)break;p=next+1;
    }
    CHECK(inode==(unsigned long)st.st_ino && (flags&O_ACCMODE)==O_RDONLY && position==0 && mount>0);
    free(description);
    free(text);
}
static void* proc_worker(void* ignored) {
    (void)ignored;int fd=open("/proc/thread-self/maps",O_RDONLY);proc_report(fd);close(fd);return NULL;
}
static void proc_views(void) {
    int fd=(int)direct_open("/proc/self/maps");proc_report(fd);close(fd);
    int directory=open("/proc/self",O_RDONLY|O_DIRECTORY);CHECK(directory>=0);
    fd=openat(directory,"maps",O_RDONLY|O_CLOEXEC);proc_report(fd);close(fd);close(directory);
    char path[128];snprintf(path,sizeof(path),"/proc/%d/maps",getpid());
    fd=open(path,O_RDONLY);proc_report(fd);close(fd);
    fd=open("/proc/thread-self/maps",O_RDONLY);proc_report(fd);close(fd);
    CHECK(!chdir("/proc/self"));fd=open("maps",O_RDONLY);proc_report(fd);close(fd);CHECK(!chdir("/"));
    unlink("/proc-maps-link");CHECK(!symlink("/proc/self/maps","/proc-maps-link"));
    fd=open("/proc-maps-link",O_RDONLY);proc_report(fd);close(fd);CHECK(!unlink("/proc-maps-link"));
    // Mapping names containing spaces and the kernel's deleted suffix survive.
    int previous=open("/proc/self/maps",O_RDONLY);CHECK(previous>=0);
    int fixture=open("/mapped file",O_RDWR|O_CREAT|O_TRUNC,0600);CHECK(fixture>=0 && !ftruncate(fixture,4096));
    struct stat mapped_stat;CHECK(!fstat(fixture,&mapped_stat));
    void* memory=mmap(NULL,4096,PROT_READ,MAP_PRIVATE,fixture,0);CHECK(memory!=MAP_FAILED);
    CHECK(!unlink("/mapped file"));close(fixture);
    fd=open("/proc/self/smaps",O_RDONLY);CHECK(fd>=0);char* smaps=proc_read_all(fd);close(fd);
    CHECK(strstr(smaps,"/mapped file (deleted)\n") && strstr(smaps,"\nRss:") && !strstr(smaps,"/files/root/"));
    char* entry=strstr(smaps,"/mapped file (deleted)\n");while(entry>smaps && entry[-1]!='\n')entry--;
    unsigned long begin=0,end=0,offset=1,inode=0;char permissions[5]={0};
    CHECK(sscanf(entry,"%lx-%lx %4s %lx %*x:%*x %lu",&begin,&end,permissions,&offset,&inode)==5);
    CHECK(begin==(unsigned long)memory && end==begin+4096 && !strcmp(permissions,"r--p") && !offset && inode==(unsigned long)mapped_stat.st_ino);
    char* before=proc_read_all(previous);CHECK(!strstr(before,"/mapped file"));free(before);close(previous);
    free(smaps);CHECK(!munmap(memory,4096));
    pthread_t workers[2];for(unsigned i=0;i<2;i++)CHECK(!pthread_create(workers+i,NULL,proc_worker,NULL));
    for(unsigned i=0;i<2;i++)CHECK(!pthread_join(workers[i],NULL));
    // Existing snapshots remain valid after a mapping disappears; reopening
    // captures the current state rather than reusing an earlier cached report.
    fd=open("/proc/self/maps",O_RDONLY);CHECK(fd>=0 && !(fcntl(fd,F_GETFD)&FD_CLOEXEC));
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        char number[32];snprintf(number,sizeof(number),"%d",fd);
        char* args[]={"probe","proc-inherited",number,NULL};char* env[]={NULL};
        execve("/system/bin/probe",args,env);CHECK(0);
    }
    int result;CHECK(waitpid(child,&result,0)==child && WIFEXITED(result) && WEXITSTATUS(result)==0);close(fd);
    fd=open("/proc/self/maps",O_RDONLY|O_CLOEXEC);CHECK(fd>=0 && (fcntl(fd,F_GETFD)&FD_CLOEXEC));close(fd);
    CHECK(open("/proc/self/maps",O_RDWR)==-1 && errno==EACCES);
    puts("PROC_VIEW_PASS inline_svc=1 dirfd=1 aliases=1 smaps=1 deleted=1 metadata=1 fdinfo=1 readv=1 pread=1 dup=1 threads=1 fork_exec=1");
}
