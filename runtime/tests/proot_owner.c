// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/capability.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/timerfd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"OWNER_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
static int owner(const char* path,uid_t uid,gid_t gid,int flags) {
    struct stat st;
    return !fstatat(AT_FDCWD,path,&st,flags) && st.st_uid==uid && st.st_gid==gid;
}
int main(int argc,char** argv) {
    if(argc==2 && !strcmp(argv[1],"lifecycle")) {
        printf("TRACE_LIFECYCLE_READY pid=%d\n",getpid());fflush(stdout);
        for(;;)pause();
    }
    const char* directory="/data/pocket-owner-fixture";
    const char* file="/data/pocket-owner-fixture/file";
    const char* link="/data/pocket-owner-fixture/link";
    if(argc==2) {
        CHECK(getuid()==1000 && owner(file,1234,2345,0));
        errno=0;execl("/pocket-missing-executable", "missing", NULL);
        CHECK(errno==ENOENT && getuid()==1000);
        gid_t groups[2];CHECK(getgroups(2,groups)==2 && groups[0]==1000 && groups[1]==3003);
        CHECK(setgroups(0,NULL)==-1 && errno==EPERM);
        errno=0;CHECK(chown(file,1000,1000)==-1 && errno==EPERM);
        char previous_path[PATH_MAX];CHECK(readlink("/proc/self/exe",previous_path,sizeof(previous_path))>0);
        int fd=open("/data/pocket-owner-fixture/app",O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
        struct stat st;CHECK(!fstat(fd,&st) && st.st_uid==1000 && st.st_gid==1000);close(fd);
        puts("OWNER_CHILD_EXEC_PASS");return 0;
    }
    CHECK(argc==1 && getuid()==0);
    char canonical[PATH_MAX],descriptor[64];
    CHECK(realpath("/data",canonical) && !strcmp(canonical,"/data"));
    int data_fd=open("/data",O_RDONLY|O_DIRECTORY);CHECK(data_fd>=0);
    CHECK(!fchdir(data_fd) && getcwd(canonical,sizeof(canonical)) && !strcmp(canonical,"/data"));
    snprintf(descriptor,sizeof(descriptor),"/proc/self/fd/%d",data_fd);
    ssize_t size=readlink(descriptor,canonical,sizeof(canonical)-1);CHECK(size>0);canonical[size]=0;
    CHECK(!strcmp(canonical,"/data") && !chdir("/"));close(data_fd);
    const char* host_canary="/data/user/0/dev.kreijstal.aosppocket.runtime/files/api28-rootfs/pocket-isolation-canary";
    errno=0;CHECK(access(host_canary,F_OK)==-1 && errno==ENOENT);
    errno=0;CHECK(unlink(host_canary)==-1 && errno==ENOENT);
    struct timezone zone={0};errno=0;CHECK(settimeofday(NULL,&zone)==-1 && errno==EPERM);
    struct __user_cap_header_struct cap_header={_LINUX_CAPABILITY_VERSION_3,0};
    struct __user_cap_data_struct cap_data[2]={{0}};
    CHECK(!syscall(SYS_capget,&cap_header,cap_data));
    CHECK(!(cap_data[0].effective|cap_data[0].permitted|cap_data[1].effective|cap_data[1].permitted));
    errno=0;execl("/pocket-missing-executable", "missing", NULL);
    CHECK(errno==ENOENT && getuid()==0);
    int realtime=timerfd_create(CLOCK_REALTIME_ALARM,TFD_CLOEXEC);CHECK(realtime>=0);close(realtime);
    int boottime=timerfd_create(CLOCK_BOOTTIME_ALARM,TFD_CLOEXEC);CHECK(boottime>=0);close(boottime);
    gid_t groups[]={1000,3003};CHECK(!setgroups(2,groups) && getgroups(0,NULL)==2);
    CHECK(!mkdir(directory,0700) || errno==EEXIST);
    unlink(link);unlink(file);unlink("/data/pocket-owner-fixture/app");
    int fd=open(file,O_CREAT|O_EXCL|O_RDWR,0600);CHECK(fd>=0);
    CHECK(!fchown(fd,1234,2345));
    struct stat st;CHECK(!fstat(fd,&st) && st.st_uid==1234 && st.st_gid==2345);
    CHECK(owner(file,1234,2345,0));
    CHECK(!fchownat(fd,"",4321,5432,AT_EMPTY_PATH));
    CHECK(owner(file,4321,5432,0));
    int dir=open(directory,O_RDONLY|O_DIRECTORY);CHECK(dir>=0);
    CHECK(!fchownat(dir,"file",1234,2345,0));close(dir);
    CHECK(owner(file,1234,2345,0));
    CHECK(!symlink("file",link));CHECK(!lchown(link,3456,4567));
    CHECK(owner(link,3456,4567,AT_SYMLINK_NOFOLLOW) && owner(link,1234,2345,0));
    int again=open(file,O_CREAT|O_TRUNC|O_RDWR,0600);CHECK(again>=0);close(again);
    CHECK(owner(file,1234,2345,0));
    CHECK(chown("/data/pocket-owner-fixture/missing",1234,2345)==-1 && errno==ENOENT);
    pid_t child=fork();CHECK(child>=0);
    if(child==0) {
        CHECK(!setgid(1000) && !setuid(1000));
        execl(argv[0],argv[0],"child",NULL);perror("owner child exec");return 1;
    }
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(owner("/data/pocket-owner-fixture/app",1000,1000,0));
    close(fd);unlink(link);unlink(file);unlink("/data/pocket-owner-fixture/app");rmdir(directory);
    puts("OWNER_METADATA_PASS fstat=1 stat=1 relative_at=1 empty_at=1 symlink=1 existing_create=1 child_exec=1 denied=1");
    return 0;
}
