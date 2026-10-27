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
#include <unistd.h>
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"CE_FAIL line=%d errno=%d %s\n",__LINE__,errno,strerror(errno));exit(1);}}while(0)
typedef struct sqlite3 sqlite3;
static int (*sqlopen)(const char*,sqlite3**,int,const char*);
static int (*sqlexec)(sqlite3*,const char*,int(*)(void*,int,char**,char**),void*,char**);
static int (*sqlclose)(sqlite3*);
static const char* (*sqlerror)(sqlite3*);
static int count(void* out,int columns,char** values,char** names){(void)names;REQUIRE(columns==1);*(int*)out=atoi(values[0]);return 0;}
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
