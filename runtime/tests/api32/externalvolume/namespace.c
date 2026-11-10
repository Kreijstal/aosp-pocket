// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <jni.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>

// Each path is inside this run's exclusive scratch directories. Retained app
// data and the other UID's canary are never modified.
JNIEXPORT jstring JNICALL
Java_dev_kreijstal_pocketvolume32_NamespaceProof_nativeRun(
        JNIEnv *env, jclass type, jstring ext, jstring internal,
        jstring peer, jstring nonce) {
    (void)type;
    const char *e=(*env)->GetStringUTFChars(env,ext,0);
    const char *i=(*env)->GetStringUTFChars(env,internal,0);
    const char *p=(*env)->GetStringUTFChars(env,peer,0);
    const char *n=(*env)->GetStringUTFChars(env,nonce,0);
    if(!e || !i || !p || !n)return 0;
    char root[4096],local[4096],a[4096],moved[4096],file[4096];
    char jump[4096],deny[4096],path[4096],link[4096],cwd[4096],buf[128];
    char result[4096],passed[2048]="",entries[4096];
    const char *stage="paths";int error=0,failedLine=0,ok=0,cleanup=1,reopenVerified=0;
    int saved=-1,dir=-1,child=-1,held=-1,duplicate=-1,probe=-1,pathDir=-1,pathFile=-1;
    int madeRoot=0,madeLocal=0,madeA=0,madeJump=0,madeDeny=0,madeFile=0;
    struct stat initial,st,directoryStat;struct statfs fs={0},dfs={0};
    const char content[]="pocket-namespace32";
#define REQUIRE(condition) do {if(!(condition)){error=errno;failedLine=__LINE__;goto done;}}while(0)
#define PATH(out,...) REQUIRE(snprintf(out,sizeof(out),__VA_ARGS__)>0 && strlen(out)<sizeof(out)-1)
#define PASS(name) do {strcat(passed,passed[0]?",\"" name "\"":"\"" name "\"");}while(0)
    PATH(root,"%s/pocket-ns-%s",e,n);PATH(local,"%s/pocket-ns-%s",i,n);
    PATH(a,"%s/A",root);PATH(moved,"%s/moved",root);PATH(file,"%s/canary",a);
    PATH(jump,"%s/jump",local);PATH(deny,"%s/peer",local);
    stage="exclusive-scratch";
    REQUIRE(mkdir(root,0700)==0);madeRoot=1;
    REQUIRE(mkdir(local,0700)==0);madeLocal=1;
    REQUIRE(mkdir(a,0700)==0);madeA=1;
    held=open(file,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);REQUIRE(held>=0);madeFile=1;
    REQUIRE(write(held,content,sizeof(content))==sizeof(content));
    REQUIRE(fsync(held)==0 && fstat(held,&initial)==0);
    dir=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC);REQUIRE(dir>=0);
    child=openat(dir,"A",O_RDONLY|O_DIRECTORY|O_CLOEXEC);REQUIRE(child>=0);
    REQUIRE(fstat(child,&directoryStat)==0 && S_ISDIR(directoryStat.st_mode));

    stage="relative-dotdot";
    probe=openat(child,"../A/./canary",O_RDONLY|O_CLOEXEC);REQUIRE(probe>=0);
    REQUIRE(fstat(probe,&st)==0 && st.st_ino==initial.st_ino && st.st_dev==initial.st_dev);
    REQUIRE(read(probe,buf,sizeof(buf))==sizeof(content) && !memcmp(buf,content,sizeof(content)));
    REQUIRE(close(probe)==0);probe=-1;PASS("relativeDotdot");

    stage="symlink-before-dotdot";
    REQUIRE(symlink(a,jump)==0);madeJump=1;
    PATH(path,"%s/../A/canary",jump);
    probe=open(path,O_RDONLY|O_CLOEXEC);REQUIRE(probe>=0);
    REQUIRE(fstat(probe,&st)==0 && st.st_ino==initial.st_ino && st.st_dev==initial.st_dev);
    REQUIRE(close(probe)==0);probe=-1;
    REQUIRE(lstat(jump,&st)==0 && S_ISLNK(st.st_mode));
    ssize_t size=readlink(jump,link,sizeof(link)-1);REQUIRE(size>0);
    link[size]=0;REQUIRE(!strcmp(link,a));
    errno=0;probe=open(jump,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);REQUIRE(probe<0 && errno==ELOOP);
    PASS("symlinkBeforeDotdot");

    stage="symlink-private-denial";
    REQUIRE(symlink(p,deny)==0);madeDeny=1;
    errno=0;probe=open(deny,O_RDONLY|O_CLOEXEC);
    REQUIRE(probe<0 && (errno==EACCES || errno==EPERM || errno==ENOENT));
    PASS("symlinkPrivateDenied");

    stage="trailing-slash";
    PATH(path,"%s/",file);errno=0;probe=open(path,O_RDONLY|O_CLOEXEC);
    REQUIRE(probe<0 && errno==ENOTDIR);
    errno=0;probe=open(file,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    REQUIRE(probe<0 && errno==ENOTDIR);PASS("trailingSlash");

    stage="poll";
    struct pollfd poller={.fd=held,.events=POLLIN|POLLOUT};
    REQUIRE(poll(&poller,1,0)==1 && (poller.revents&(POLLIN|POLLOUT))==(POLLIN|POLLOUT));
    REQUIRE(!(poller.revents&(POLLERR|POLLHUP|POLLNVAL)));
    stage="fstatfs-file";REQUIRE(fstatfs(held,&fs)==0);
    stage="fstatfs-directory";REQUIRE(fstatfs(child,&dfs)==0);
    stage="fstatfs-coherence";
    REQUIRE(fs.f_type==dfs.f_type && fs.f_bsize==dfs.f_bsize && fs.f_bsize>0 &&
        fs.f_blocks>0 && fs.f_bavail<=fs.f_bfree && fs.f_bfree<=fs.f_blocks);
    PASS("pollAndFstatfs");

    stage="renamed-directory";
    duplicate=fcntl(child,F_DUPFD_CLOEXEC,3);REQUIRE(duplicate>=0);
    REQUIRE(fcntl(duplicate,F_GETFD)&FD_CLOEXEC);
    REQUIRE(rename(a,moved)==0);madeA=2;
    PATH(file,"%s/canary",moved);
    probe=openat(duplicate,"canary",O_RDONLY|O_CLOEXEC);REQUIRE(probe>=0);
    REQUIRE(fstat(probe,&st)==0 && st.st_ino==initial.st_ino && st.st_dev==initial.st_dev);
    REQUIRE(close(probe)==0);probe=-1;
    PATH(path,"/proc/self/fd/%d",child);
    size=readlink(path,link,sizeof(link)-1);REQUIRE(size>0);
    link[size]=0;REQUIRE(!strcmp(link,moved));
    REQUIRE(stat(path,&st)==0 && S_ISDIR(st.st_mode) && st.st_ino==directoryStat.st_ino && st.st_dev==directoryStat.st_dev);
    REQUIRE(lstat(path,&st)==0 && S_ISLNK(st.st_mode));
    stage="proc-reopen-live-directory";
    REQUIRE(syscall(__NR_getdents64,child,entries,sizeof(entries))>0);
    off_t directoryOffset=lseek(child,0,SEEK_CUR);REQUIRE(directoryOffset>0);
    probe=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);REQUIRE(probe>=0);
    REQUIRE(fcntl(probe,F_GETFD)&FD_CLOEXEC);
    REQUIRE(syscall(__NR_getdents64,probe,entries,sizeof(entries))>0);
    REQUIRE(lseek(child,0,SEEK_CUR)==directoryOffset);
    REQUIRE(close(probe)==0);probe=-1;
    pathFile=open(file,O_PATH|O_CLOEXEC);REQUIRE(pathFile>=0);
    stage="proc-reopen-live-file";
    off_t originalOffset=lseek(held,0,SEEK_CUR);REQUIRE(originalOffset>=0);
    PATH(path,"/proc/self/fd/%d",held);
    probe=open(path,O_RDONLY|O_CLOEXEC);REQUIRE(probe>=0);
    REQUIRE(lseek(probe,0,SEEK_CUR)==0 && read(probe,buf,sizeof(buf))==sizeof(content) && !memcmp(buf,content,sizeof(content)));
    REQUIRE(lseek(held,0,SEEK_CUR)==originalOffset);
    REQUIRE(close(probe)==0);probe=-1;
    PATH(path,"/proc/self/fd/%d",pathFile);
    probe=open(path,O_RDONLY|O_CLOEXEC);REQUIRE(probe>=0);
    REQUIRE(read(probe,buf,sizeof(buf))==sizeof(content) && !memcmp(buf,content,sizeof(content)));
    REQUIRE(close(probe)==0);probe=-1;
    PATH(path,"/proc/self/fd/%d/canary",duplicate);
    probe=open(path,O_RDONLY|O_CLOEXEC);REQUIRE(probe>=0);
    REQUIRE(fstat(probe,&st)==0 && st.st_ino==initial.st_ino && st.st_dev==initial.st_dev);
    REQUIRE(close(probe)==0);probe=-1;
    PASS("renamedDirectoryAndProcFd");

    stage="cwd-relative";
    saved=open(".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);REQUIRE(saved>=0);
    PATH(path,"/proc/self/fd/%d",duplicate);
    REQUIRE(chdir(path)==0 && getcwd(cwd,sizeof(cwd)) && !strcmp(cwd,moved));
    REQUIRE(fchdir(saved)==0);
    REQUIRE(fchdir(duplicate)==0 && getcwd(cwd,sizeof(cwd)) && !strcmp(cwd,moved));
    probe=open("../moved/canary",O_RDONLY|O_CLOEXEC);REQUIRE(probe>=0);
    REQUIRE(fstat(probe,&st)==0 && st.st_ino==initial.st_ino && st.st_dev==initial.st_dev);
    REQUIRE(close(probe)==0);probe=-1;REQUIRE(fchdir(saved)==0);PASS("cwdRelative");

    stage="unlinked-held-file";
    REQUIRE(unlink(file)==0);madeFile=0;
    REQUIRE(fstat(held,&st)==0 && st.st_ino==initial.st_ino && st.st_nlink==0);
    REQUIRE(pread(held,buf,sizeof(buf),0)==sizeof(content) && !memcmp(buf,content,sizeof(content)));
    PATH(path,"/proc/self/fd/%d",held);
    REQUIRE(stat(path,&st)==0 && S_ISREG(st.st_mode) && st.st_ino==initial.st_ino && st.st_dev==initial.st_dev && st.st_nlink==0);
    errno=0;REQUIRE(chdir(path)<0 && errno==ENOTDIR);
    size=readlink(path,link,sizeof(link)-1);REQUIRE(size>0);link[size]=0;
    PATH(path,"%s (deleted)",file);REQUIRE(!strcmp(link,path));
    stage="proc-reopen-unlinked-file";
    for(unsigned source=0;source<2;source++) {
        PATH(path,"/proc/self/fd/%d",source?pathFile:held);
        probe=open(path,O_RDONLY|O_CLOEXEC);REQUIRE(probe>=0);
        REQUIRE(fstat(probe,&st)==0 && st.st_ino==initial.st_ino && st.st_dev==initial.st_dev && st.st_nlink==0);
        REQUIRE(read(probe,buf,sizeof(buf))==sizeof(content) && !memcmp(buf,content,sizeof(content)));
        REQUIRE(lseek(held,0,SEEK_CUR)==originalOffset);
        REQUIRE(close(probe)==0);probe=-1;
        probe=open(path,O_PATH|O_CLOEXEC);REQUIRE(probe>=0);
        REQUIRE(fstat(probe,&st)==0 && S_ISREG(st.st_mode) && st.st_ino==initial.st_ino && !st.st_nlink);
        REQUIRE(close(probe)==0);probe=-1;
    }
    PASS("unlinkedHeldFile");

    stage="unlinked-held-directory";
    pathDir=open(moved,O_PATH|O_DIRECTORY|O_CLOEXEC);REQUIRE(pathDir>=0);
    REQUIRE(rmdir(moved)==0);madeA=0;
    REQUIRE(fstat(duplicate,&st)==0 && S_ISDIR(st.st_mode) && st.st_nlink==0);
    REQUIRE(fchdir(duplicate)==0);errno=0;
    REQUIRE(!getcwd(cwd,sizeof(cwd)) && errno==ENOENT);
    errno=0;probe=openat(duplicate,"new",O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);
    REQUIRE(probe<0 && errno==ENOENT);
    REQUIRE(fchdir(saved)==0);
    REQUIRE(fstat(pathDir,&st)==0 && S_ISDIR(st.st_mode) && st.st_nlink==0);
    REQUIRE(fchdir(pathDir)==0);errno=0;
    REQUIRE(!getcwd(cwd,sizeof(cwd)) && errno==ENOENT);
    REQUIRE(fchdir(saved)==0);
    PATH(path,"/proc/self/fd/%d",duplicate);
    REQUIRE(stat(path,&st)==0 && S_ISDIR(st.st_mode) && st.st_ino==directoryStat.st_ino && st.st_dev==directoryStat.st_dev && st.st_nlink==0);
    REQUIRE(chdir(path)==0);errno=0;
    REQUIRE(!getcwd(cwd,sizeof(cwd)) && errno==ENOENT);
    REQUIRE(fchdir(saved)==0);
    size=readlink(path,link,sizeof(link)-1);REQUIRE(size>0);link[size]=0;
    PATH(path,"%s (deleted)",moved);REQUIRE(!strcmp(link,path));
    stage="proc-reopen-unlinked-directory";
    for(unsigned source=0;source<2;source++) {
        PATH(path,"/proc/self/fd/%d",source?pathDir:duplicate);
        probe=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);REQUIRE(probe>=0);
        REQUIRE(fstat(probe,&st)==0 && S_ISDIR(st.st_mode) && st.st_ino==directoryStat.st_ino && !st.st_nlink);
        REQUIRE(syscall(__NR_getdents64,probe,entries,sizeof(entries))==0);
        REQUIRE(fchdir(probe)==0);errno=0;
        REQUIRE(!getcwd(cwd,sizeof(cwd)) && errno==ENOENT);
        REQUIRE(fchdir(saved)==0 && close(probe)==0);probe=-1;
    }
    reopenVerified=1;
    PASS("unlinkedHeldDirectory");ok=1;
done:
    // Restore process cwd before returning to Android, including failure paths.
    if(saved>=0 && fchdir(saved))cleanup=0;
    int fds[]={probe,pathFile,pathDir,duplicate,held,child,dir,saved};
    for(unsigned x=0;x<sizeof(fds)/sizeof(fds[0]);x++)if(fds[x]>=0 && close(fds[x]))cleanup=0;
    if(madeDeny && unlink(deny))cleanup=0;
    if(madeJump && unlink(jump))cleanup=0;
    if(madeFile && unlink(file))cleanup=0;
    if(madeA && rmdir(madeA==2?moved:a))cleanup=0;
    if(madeLocal && rmdir(local))cleanup=0;
    if(madeRoot && rmdir(root))cleanup=0;
    snprintf(result,sizeof(result),
        "{\"validation\":\"%s\",\"stage\":\"%s\",\"errno\":%d,\"failureLine\":%d,\"uid\":%u,"
        "\"fileFilesystemType\":%lld,\"directoryFilesystemType\":%lld,\"fileBlockSize\":%lld,\"directoryBlockSize\":%lld,"
        "\"passed\":[%s],\"cleanupComplete\":%s,\"directInProcessSyscalls\":true,\"procFdReopenVerified\":%s}",
        ok&&cleanup?"PASS":"FAIL",stage,error,failedLine,(unsigned)getuid(),
        (long long)fs.f_type,(long long)dfs.f_type,(long long)fs.f_bsize,(long long)dfs.f_bsize,passed,cleanup?"true":"false",reopenVerified?"true":"false");
    (*env)->ReleaseStringUTFChars(env,ext,e);(*env)->ReleaseStringUTFChars(env,internal,i);
    (*env)->ReleaseStringUTFChars(env,peer,p);(*env)->ReleaseStringUTFChars(env,nonce,n);
    return (*env)->NewStringUTF(env,result);
}
