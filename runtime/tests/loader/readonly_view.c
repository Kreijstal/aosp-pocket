// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char* fixture="/system/etc/pocket-readonly-fixture.txt";
#define CHECK(condition) do {if(!(condition)){fprintf(stderr,"line %d: %s errno=%d (%s)\n",__LINE__,#condition,errno,strerror(errno));return 1;}}while(0)
#define READONLY(operation) do {errno=0;CHECK((operation)==-1 && errno==EROFS);}while(0)
static int verify(void) {
    // The old loader fails here, before any attempted fixture mutation.
    READONLY(access(fixture,W_OK));READONLY(access("/system/etc",W_OK));
    CHECK(access("/system/etc/pocket-readonly-absent",W_OK)==-1 && errno==ENOENT);
    int fd=open(fixture,O_RDONLY|O_CLOEXEC);CHECK(fd>=0);
    char bytes[32]={0};CHECK(read(fd,bytes,sizeof(bytes))==17 && !strcmp(bytes,"readonly-fixture\n"));
    READONLY(fchmod(fd,0600));READONLY(fchown(fd,39999,39999));READONLY(ftruncate(fd,0));CHECK(!close(fd));
    fd=open(fixture,O_RDONLY|O_CREAT|O_CLOEXEC,0600);CHECK(fd>=0);CHECK(!close(fd));
    READONLY(open(fixture,O_RDWR|O_CLOEXEC));READONLY(truncate(fixture,0));READONLY(chmod(fixture,0600));
    READONLY(unlink(fixture));READONLY(rename(fixture,"/tmp/readonly-moved-fixture"));
    READONLY(unlink("/.pocket-readonly-system"));
    READONLY(open("/system/etc/pocket-readonly-new",O_WRONLY|O_CREAT|O_CLOEXEC,0600));
    READONLY(mkdir("/system/etc/pocket-readonly-dir",0700));
    int dir=open("/system",O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(dir>=0);
    READONLY(faccessat(dir,"etc/../etc/pocket-readonly-fixture.txt",W_OK,0));
    READONLY(openat(dir,"etc/pocket-readonly-fixture.txt",O_WRONLY|O_CLOEXEC));
    READONLY(openat(dir,"etc/pocket-readonly-new",O_CREAT|O_WRONLY|O_CLOEXEC,0600));
    char magic[128];snprintf(magic,sizeof(magic),"/proc/self/fd/%d/etc/pocket-readonly-fixture.txt",dir);
    READONLY(open(magic,O_WRONLY|O_CLOEXEC));CHECK(!close(dir));
    char data[128],linkname[128];snprintf(data,sizeof(data),"/tmp/readonly-data-%d",getpid());
    snprintf(linkname,sizeof(linkname),"/tmp/readonly-link-%d",getpid());
    fd=open(data,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);CHECK(fd>=0);
    CHECK(write(fd,"data",4)==4 && !ftruncate(fd,2) && !fchmod(fd,0640));CHECK(!close(fd));
    CHECK(!symlink(fixture,linkname));READONLY(open(linkname,O_WRONLY|O_CLOEXEC));CHECK(!unlink(linkname));
    CHECK(!symlink("/system/etc/pocket-readonly-new",linkname));
    READONLY(open(linkname,O_WRONLY|O_CREAT|O_CLOEXEC,0600));CHECK(!unlink(linkname));
    CHECK(!symlink("/system/etc",linkname));
    char child[160];snprintf(child,sizeof(child),"%s/pocket-readonly-new",linkname);
    READONLY(open(child,O_WRONLY|O_CREAT|O_CLOEXEC,0600));CHECK(!unlink(linkname));
    READONLY(link(fixture,linkname));READONLY(rename(data,"/system/etc/pocket-readonly-new"));
    CHECK(!access(data,W_OK));CHECK(!unlink(data));
    return 0;
}
int main(int argc,char** argv) {
    if(argc==2 && !strcmp(argv[1],"--expect-writable")) {
        CHECK(!access(fixture,W_OK));puts("LOADER_READONLY_DISABLED_PASS");return 0;
    }
    CHECK(argc==1);CHECK(getuid()==0);CHECK(!verify());
    pid_t child=fork();CHECK(child>=0);
    if(!child){if(setgid(39999) || setuid(39999))_exit(2);_exit(verify());}
    int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
    puts("LOADER_READONLY_VIEW_PASS uids=0,39999 reads=allowed writes=EROFS data=writable symlinks=resolved dirfds=resolved");
    return 0;
}
