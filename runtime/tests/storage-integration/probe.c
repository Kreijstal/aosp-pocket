// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"CE_FAIL line=%d errno=%d %s\n",__LINE__,errno,strerror(errno));exit(1);}}while(0)
typedef struct sqlite3 sqlite3;
static int (*sqlopen)(const char*,sqlite3**,int,const char*);
static int (*sqlexec)(sqlite3*,const char*,int(*)(void*,int,char**,char**),void*,char**);
static int (*sqlclose)(sqlite3*);
static const char* (*sqlerror)(sqlite3*);
static int count(void* out,int columns,char** values,char** names){(void)names;REQUIRE(columns==1);*(int*)out=atoi(values[0]);return 0;}
static void removed_directory_test(void) {
    const char* path="/data/user/0/proc-removed-dir";
    REQUIRE(mkdir(path,0700)==0);
    int saved=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);REQUIRE(saved>=0);
    int readable=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);REQUIRE(readable>=0);
    int metadata=open(path,O_PATH|O_DIRECTORY|O_CLOEXEC);REQUIRE(metadata>=0);
    struct stat before,after;REQUIRE(fstat(readable,&before)==0 && S_ISDIR(before.st_mode));
    REQUIRE(rmdir(path)==0);
    for(unsigned source=0;source<2;source++) {
        int fd=source?metadata:readable;
        REQUIRE(fstat(fd,&after)==0 && after.st_ino==before.st_ino && !after.st_nlink);
        errno=0;int search=faccessat(fd,".",X_OK,0);
        if(search)fprintf(stderr,"CE_REMOVED_DIR_SEARCH_ERROR source=%s errno=%d\n",source?"metadata":"data",errno);
        REQUIRE(search==0 && fchdir(fd)==0);
        char cwd[4096];errno=0;REQUIRE(!getcwd(cwd,sizeof(cwd)) && errno==ENOENT);
        REQUIRE(fchdir(saved)==0);
        errno=0;int invalid=openat(fd,"new",O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);REQUIRE(invalid<0 && errno==ENOENT);
        char link[64],entries[256];snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
        int fresh=open(link,O_RDONLY|O_DIRECTORY|O_CLOEXEC);REQUIRE(fresh>=0);
        REQUIRE(fstat(fresh,&after)==0 && after.st_ino==before.st_ino && !after.st_nlink);
        REQUIRE(syscall(SYS_getdents64,fresh,entries,sizeof(entries))==0 && close(fresh)==0);
    }
    for(unsigned grant=0;grant<2;grant++) {
        REQUIRE(fchmod(readable,grant?0711:0700)==0);
        pid_t child=fork();REQUIRE(child>=0);
        if(!child) {
            if(setgid(39998) || setuid(39998))_exit(2);
            errno=0;int result=faccessat(metadata,".",X_OK,0);
            _exit(grant?(result==0?0:3):(result<0 && errno==EACCES?0:4));
        }
        int status=0;REQUIRE(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    REQUIRE(close(metadata)==0 && close(readable)==0 && close(saved)==0);
    puts("CE_REMOVED_DIRECTORY_PASS descriptor_search=1 current_uid_permission=1 cwd_enoent=1 new_child_enoent=1 proc_reopen_same_inode=1 empty_readdir=1");
}
static void proc_reopen_test(void) {
    const char* directory="/data/user/0/proc-reopen";
    const char* name="/data/user/0/proc-reopen/file";
    REQUIRE(mkdir(directory,0700)==0);
    int held=open(name,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);REQUIRE(held>=0);
    const char bytes[]="original-unlinked";REQUIRE(write(held,bytes,sizeof(bytes))==sizeof(bytes));
    off_t offset=lseek(held,0,SEEK_CUR);REQUIRE(offset==sizeof(bytes));
    int metadata=open(name,O_PATH|O_CLOEXEC);REQUIRE(metadata>=0);
    struct stat initial,actual;REQUIRE(fstat(held,&initial)==0);
    REQUIRE(unlink(name)==0);
    int replacement=open(name,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);REQUIRE(replacement>=0);
    REQUIRE(write(replacement,"replacement",11)==11);
    for(unsigned source=0;source<2;source++) {
        char path[64],contents[64]={0};snprintf(path,sizeof(path),"/proc/self/fd/%d",source?metadata:held);
        errno=0;int fresh=open(path,O_RDONLY|O_CLOEXEC);
        if(fresh<0)fprintf(stderr,"CE_PROC_REOPEN_ERROR source=%s errno=%d\n",source?"metadata":"data",errno);
        REQUIRE(fresh>=0 && (fcntl(fresh,F_GETFD)&FD_CLOEXEC));
        REQUIRE(fstat(fresh,&actual)==0 && actual.st_dev==initial.st_dev && actual.st_ino==initial.st_ino && !actual.st_nlink);
        REQUIRE(read(fresh,contents,sizeof(contents))==sizeof(bytes) && !memcmp(contents,bytes,sizeof(bytes)));
        REQUIRE(lseek(held,0,SEEK_CUR)==offset && close(fresh)==0);
    }
    // A transferred descriptor still needs current inode access permission.
    // Search permission on a replacement pathname grants no authority.
    for(unsigned grant=0;grant<2;grant++) {
        REQUIRE(fchmod(held,grant?0644:0600)==0);
        pid_t child=fork();REQUIRE(child>=0);
        if(!child) {
            if(setgid(39998) || setuid(39998))_exit(2);
            char path[64],contents[64]={0};snprintf(path,sizeof(path),"/proc/self/fd/%d",held);
            errno=0;int fresh=open(path,O_RDONLY|O_CLOEXEC);
            if(!grant)_exit(fresh<0 && errno==EACCES?0:3);
            if(fresh<0 || read(fresh,contents,sizeof(contents))!=sizeof(bytes) || memcmp(contents,bytes,sizeof(bytes)))_exit(4);
            close(fresh);errno=0;fresh=open(name,O_RDONLY|O_CLOEXEC);
            _exit(fresh<0 && errno==EACCES?0:5);
        }
        int status=0;REQUIRE(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    REQUIRE(close(metadata)==0 && close(held)==0 && close(replacement)==0 && unlink(name)==0 && rmdir(directory)==0);
    puts("CE_PROC_REOPEN_PASS actual_appfuse=1 removed_inode=1 replacement_not_read=1 independent_offset=1 current_uid_permission=1 parent_search_not_inherited=1");
}
static void metadata_test(int verify) {
    char label[256],other[256],path[4096],link[128];
    ssize_t length=getxattr("/data/user/0","security.selinux",label,sizeof(label));REQUIRE(length>0);
    int fd=open("/data/user/0/seed",O_PATH|O_CLOEXEC);REQUIRE(fd>=0);
    REQUIRE(getxattr("/data/user/0/seed","security.selinux",other,sizeof(other))==length && !memcmp(label,other,length));
    snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);ssize_t n=readlink(link,path,sizeof(path)-1);REQUIRE(n>0);path[n]=0;
    if(strcmp(path,"/data/user/0/seed"))fprintf(stderr,"CE_METADATA_PROC_FD expected=/data/user/0/seed actual=%s\n",path);
    REQUIRE(!strcmp(path,"/data/user/0/seed"));REQUIRE(close(fd)==0);
    fd=open("/data/user/0",O_PATH|O_DIRECTORY|O_CLOEXEC);REQUIRE(fd>=0);
    snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);n=readlink(link,path,sizeof(path)-1);REQUIRE(n>0);path[n]=0;
    REQUIRE(!strcmp(path,"/data/user/0"));REQUIRE(close(fd)==0);
    REQUIRE(realpath("/data/user/0/seed",path) && !strcmp(path,"/data/user/0/seed"));
    if(!verify) {
        REQUIRE(mkdir("/data/user/0/metadata-child",0700)==0);
        fd=open("/data/user/0/metadata-child/file",O_CREAT|O_RDWR,0600);REQUIRE(fd>=0);
        REQUIRE(fgetxattr(fd,"security.selinux",other,sizeof(other))==length && !memcmp(label,other,length));
        REQUIRE(fsetxattr(fd,"security.selinux","untrusted-label",16,0)<0 && errno==EOPNOTSUPP);
        REQUIRE(fsync(fd)==0 && close(fd)==0);
    }
    REQUIRE(getxattr("/data/user/0/metadata-child/file","security.selinux",other,sizeof(other))==length && !memcmp(label,other,length));
    puts("CE_METADATA_PASS inherited_host_label=persistent_readonly proc_fd=virtual canonical_path=correct");
}
static void sql(sqlite3* db,const char* text){int rc=sqlexec(db,text,0,0,0);if(rc){fprintf(stderr,"SQL_FAIL code=%d %s\n",rc,sqlerror(db));exit(1);}}
static void sqlite_test(int verify) {
    void* library=dlopen("libsqlite.so",RTLD_NOW);if(!library){fprintf(stderr,"SQL_LOAD %s\n",dlerror());exit(1);}
    sqlopen=dlsym(library,"sqlite3_open_v2");sqlexec=dlsym(library,"sqlite3_exec");sqlclose=dlsym(library,"sqlite3_close");sqlerror=dlsym(library,"sqlite3_errmsg");REQUIRE(sqlopen&&sqlexec&&sqlclose&&sqlerror);
    sqlite3* db=NULL;REQUIRE(sqlopen("/data/user/0/app/database",&db,6,0)==0);
    if(!verify){sql(db,"PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; CREATE TABLE entries(value); INSERT INTO entries VALUES(17);");}
    int value=0;REQUIRE(sqlexec(db,"SELECT value FROM entries;",count,&value,0)==0 && value==17);
    if(!verify) {
        sql(db,"BEGIN IMMEDIATE;");pid_t child=fork();REQUIRE(child>=0);
        if(!child){sqlite3* other=NULL;if(sqlopen("/data/user/0/app/database",&other,6,0)!=0)_exit(2);int rc=sqlexec(other,"BEGIN IMMEDIATE;",0,0,0);sqlclose(other);_exit(rc==5?0:3);}
        int status;REQUIRE(waitpid(child,&status,0)==child && WIFEXITED(status)&&WEXITSTATUS(status)==0);sql(db,"COMMIT; PRAGMA wal_checkpoint(TRUNCATE);");
    }
    REQUIRE(sqlclose(db)==0);dlclose(library);puts("CE_SQLITE_PASS WAL=1 FULL=1 cross_process_writer_exclusion=1");
}
int main(int argc,char** argv) {
    setbuf(stdout,NULL);REQUIRE(argc==2);
    if(!strcmp(argv[1],"removed-dir")){removed_directory_test();return 0;}
    if(!strcmp(argv[1],"proc-reopen")){proc_reopen_test();return 0;}
    if(!strcmp(argv[1],"metadata") || !strcmp(argv[1],"metadata-verify")){metadata_test(!strcmp(argv[1],"metadata-verify"));return 0;}
    if(!strncmp(argv[1],"vendor-",7)) {
        REQUIRE(syscall(SYS_getpid,0x504c2001UL,5UL)==0x504c3202UL);
        if(!strcmp(argv[1],"vendor-locked")) {
            int fd=open("/data/vendor_ce/0/seed",O_RDONLY);REQUIRE(fd<0 && errno==EACCES);
            fd=open("/data/vendor_ce/1/plaintext-fallback",O_CREAT|O_RDWR,0600);REQUIRE(fd<0 && errno==EOPNOTSUPP);
            puts("CE_VENDOR_LOCKED_PASS loader_layout=12L additional_users_fail_closed=1");return 0;
        }
        int fd=open("/data/vendor_ce/0/seed",O_RDONLY);REQUIRE(fd>=0);char bytes[32]={0};
        REQUIRE(read(fd,bytes,sizeof(bytes))==23 && !strcmp(bytes,"vendor-migration-canary"));REQUIRE(close(fd)==0);
        int verify=!strcmp(argv[1],"vendor-verify");
        if(!verify) {
            REQUIRE(mkdir("/data/vendor_ce/0/drm",0700)==0);
            fd=open("/data/vendor_ce/0/drm/key",O_RDWR|O_CREAT|O_EXCL,0600);REQUIRE(fd>=0);
            REQUIRE(write(fd,"vendor-key-canary",17)==17 && fsync(fd)==0 && close(fd)==0);
        }
        REQUIRE(chdir("/data/vendor_ce/0/drm")==0);char cwd[128];REQUIRE(getcwd(cwd,sizeof(cwd)) && !strcmp(cwd,"/data/vendor_ce/0/drm"));
        fd=openat(AT_FDCWD,"key",O_RDONLY);REQUIRE(fd>=0);memset(bytes,0,sizeof(bytes));
        REQUIRE(read(fd,bytes,sizeof(bytes))==17 && !strcmp(bytes,"vendor-key-canary"));struct stat st;
        REQUIRE(fstat(fd,&st)==0 && st.st_uid==0 && (st.st_mode&0777)==0600 && close(fd)==0 && chdir("/")==0);
        pid_t app=fork();REQUIRE(app>=0);
        if(!app){if(setgid(19990)||setuid(19990))_exit(2);fd=open("/data/vendor_ce/0/drm/key",O_RDONLY);_exit(fd<0 && errno==EACCES?0:3);}
        int status;REQUIRE(waitpid(app,&status,0)==app && WIFEXITED(status) && WEXITSTATUS(status)==0);
        puts(verify?"CE_VENDOR_REOPEN_PASS persistence=1 owner=0 private_mode=0600 app_access=denied":"CE_VENDOR_FILESYSTEM_PASS migration=1 read_write=1 fsync=1 cwd=virtual app_access=denied");return 0;
    }
    if(!strcmp(argv[1],"locked")){int fd=open("/data/user/0/private",O_RDONLY);REQUIRE(fd<0 && errno==EACCES);fd=open("/data/user/1/plaintext-fallback",O_CREAT|O_RDWR,0600);REQUIRE(fd<0 && errno==EOPNOTSUPP);puts("CE_LOCKED_PASS additional_users_fail_closed=1");return 0;}
    if(!strcmp(argv[1],"verify")){int fd=open("/data/user/0/app/renamed",O_RDONLY);REQUIRE(fd>=0);char data[8]={0};REQUIRE(read(fd,data,8)==8 && data[1]=='Q');close(fd);sqlite_test(1);
        fd=open("/data/user/0/app/buffered/value",O_RDONLY);REQUIRE(fd>=0);REQUIRE(read(fd,data,8)==5 && !memcmp(data,"batch",5));struct stat st;REQUIRE(fstat(fd,&st)==0 && (st.st_mode&0777)==0640);
        char attr[8]={0};REQUIRE(fgetxattr(fd,"user.batch",attr,sizeof(attr))==3 && !strcmp(attr,"ok"));REQUIRE(close(fd)==0);
        puts("CE_REOPEN_PASS buffered_shutdown_flush=1");return 0;}
    int seed=open("/data/user/0/seed",O_RDONLY);REQUIRE(seed>=0);char migrated[32]={0};REQUIRE(read(seed,migrated,sizeof(migrated))==16 && !strcmp(migrated,"migration-canary"));close(seed);
    REQUIRE(mkdir("/data/user/0/app",0700)==0);int fd=open("/data/user/0/app/file",O_CREAT|O_RDWR|O_CLOEXEC,0600);REQUIRE(fd>=0);
    REQUIRE(ftruncate(fd,8192)==0);struct stat st;REQUIRE(fstat(fd,&st)==0 && st.st_size==8192 && st.st_uid==0);
    REQUIRE(fchown(fd,19990,19990)==0);
    pid_t identity=fork();REQUIRE(identity>=0);
    if(!identity){struct stat owned;if(setgid(19990)||setuid(19990))_exit(2);if(fstat(fd,&owned)||owned.st_uid!=19990)_exit(3);if(fstatat(fd,"",&owned,AT_EMPTY_PATH)||owned.st_uid!=19990)_exit(4);_exit(0);}
    int identity_status;REQUIRE(waitpid(identity,&identity_status,0)==identity && WIFEXITED(identity_status)&&WEXITSTATUS(identity_status)==0);REQUIRE(fchown(fd,0,0)==0);
    struct statvfs space;REQUIRE(statvfs("/data/user/0/app",&space)==0 && space.f_bsize>0);REQUIRE(fstatvfs(fd,&space)==0 && space.f_bsize>0);
    REQUIRE(setxattr("/data/user/0/app/file","user.test","xattr-canary",12,0)==0);char attr[32]={0};REQUIRE(fgetxattr(fd,"user.test",attr,sizeof(attr))==12 && !strcmp(attr,"xattr-canary"));
    char initial[8192];memset(initial,'A',sizeof(initial));REQUIRE(write(fd,initial,sizeof(initial))==sizeof(initial));REQUIRE(fsync(fd)==0);
    char* map=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);REQUIRE(map!=MAP_FAILED && map[0]=='A');map[1]='Q';REQUIRE(msync(map,4096,MS_SYNC)==0);REQUIRE(munmap(map,4096)==0);
    struct flock lock={.l_type=F_WRLCK,.l_whence=SEEK_SET,.l_start=21,.l_len=1};REQUIRE(fcntl(fd,F_SETLK,&lock)==0);
    pid_t child=fork();REQUIRE(child>=0);if(!child){int other=open("/data/user/0/app/file",O_RDWR);if(other<0)_exit(2);int rc=fcntl(other,F_SETLK,&lock);_exit(rc<0&&(errno==EAGAIN||errno==EACCES)?0:3);}
    int status;REQUIRE(waitpid(child,&status,0)==child && WIFEXITED(status)&&WEXITSTATUS(status)==0);lock.l_type=F_UNLCK;REQUIRE(fcntl(fd,F_SETLK,&lock)==0);
    REQUIRE(ftruncate(fd,8)==0);REQUIRE(fstat(fd,&st)==0 && st.st_size==8);REQUIRE(fsync(fd)==0);int duplicate=dup(fd);REQUIRE(duplicate>=0);REQUIRE(close(fd)==0);REQUIRE(close(duplicate)==0);
    REQUIRE(rename("/data/user/0/app/file","/data/user/0/app/renamed")==0);REQUIRE(link("/data/user/0/app/renamed","/data/user/0/app/hardlink")==0);
    REQUIRE(symlink("renamed","/data/user/0/app/symlink")==0);fd=open("/data/data/app/symlink",O_RDONLY);REQUIRE(fd>=0);REQUIRE(close(fd)==0);
    REQUIRE(chdir("/data/user/0/app")==0);char cwd[1024];char* actual=getcwd(cwd,sizeof(cwd));
    if(!actual || strcmp(cwd,"/data/user/0/app"))fprintf(stderr,"CE_CWD value=%s errno=%d\n",actual?actual:"<failed>",errno);
    REQUIRE(actual!=NULL && !strcmp(cwd,"/data/user/0/app"));
    DIR* directory=opendir(".");REQUIRE(directory);int seen=0;struct dirent* entry;while((entry=readdir(directory)))if(!strcmp(entry->d_name,"renamed"))seen++;REQUIRE(seen==1);closedir(directory);
    fd=openat(AT_FDCWD,"renamed",O_RDONLY);REQUIRE(fd>=0);close(fd);REQUIRE(chdir("/")==0);
    int dirfd=open("/data/user/0/app",O_RDONLY|O_DIRECTORY);REQUIRE(dirfd>=0);
    char procpath[128];snprintf(procpath,sizeof(procpath),"/proc/self/fd/%d/proc-alias",dirfd);
    fd=open(procpath,O_CREAT|O_RDWR,0600);REQUIRE(fd>=0);REQUIRE(write(fd,"alias",5)==5);REQUIRE(fsync(fd)==0);close(fd);close(dirfd);
    fd=open("/data/user/0/app/proc-alias",O_RDONLY);REQUIRE(fd>=0);char alias[6]={0};REQUIRE(read(fd,alias,5)==5 && !strcmp(alias,"alias"));close(fd);
    // An ARM64 function returning 42 exercises executable CE mappings without
    // writing a plaintext executable to disk.
    const unsigned code[]={0x52800540,0xd65f03c0};fd=open("/data/user/0/app/code",O_CREAT|O_RDWR,0600);REQUIRE(fd>=0);REQUIRE(write(fd,code,sizeof(code))==sizeof(code));REQUIRE(fsync(fd)==0);
    void* execute=mmap(0,4096,PROT_READ|PROT_EXEC,MAP_PRIVATE,fd,0);REQUIRE(execute!=MAP_FAILED);REQUIRE(((int(*)(void))execute)()==42);munmap(execute,4096);close(fd);
    sqlite_test(0);
    // No explicit fsync: normal shutdown must drain these buffered changes.
    REQUIRE(mkdir("/data/user/0/app/buffered",0700)==0);fd=open("/data/user/0/app/buffered/value",O_CREAT|O_RDWR,0600);REQUIRE(fd>=0);
    REQUIRE(write(fd,"batch",5)==5);REQUIRE(fchmod(fd,0640)==0);REQUIRE(fsetxattr(fd,"user.batch","ok",3,0)==0);REQUIRE(close(fd)==0);
    puts("CE_FILESYSTEM_PASS mmap=1 truncation=1 locks=1 directories=1 links=1 executable_memory=1");return 0;
}
