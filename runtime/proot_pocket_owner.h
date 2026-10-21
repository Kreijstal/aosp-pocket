// SPDX-License-Identifier: GPL-3.0-only
// Included by the optional PRoot fake-id extension, never by guest Bionic.
#include <fcntl.h>

typedef struct { uint32_t magic; uid_t uid; gid_t gid; } PocketOwner;

static bool pocket_owner_enabled(const Tracee* tracee) {
#if defined(__aarch64__)
    const char* enabled=getenv("POCKET_PROOT_OWNER");
    return enabled && !strcmp(enabled,"1") && !is_32on64_mode(tracee);
#else
    (void)tracee;return false;
#endif
}

static int pocket_owner_name(const Tracee* tracee,const struct stat* st,char* path) {
    int n=snprintf(path,PATH_MAX,"%s/.pocket-owner/%llx-%llx",get_root(tracee),
        (unsigned long long)st->st_dev,(unsigned long long)st->st_ino);
    return n<0 || n>=PATH_MAX ? -ENAMETOOLONG : 0;
}

static PocketOwner pocket_owner_read(const Tracee* tracee,const struct stat* st) {
    PocketOwner owner={0x504f434b,0,0},found;
    char path[PATH_MAX];
    if(pocket_owner_name(tracee,st,path))return owner;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd>=0) {
        if(read(fd,&found,sizeof(found))==sizeof(found) && found.magic==owner.magic)owner=found;
        close(fd);
    }
    return owner;
}

static int pocket_owner_write(const Tracee* tracee,const struct stat* st,PocketOwner owner) {
    char path[PATH_MAX],directory[PATH_MAX],temporary[PATH_MAX];
    if(pocket_owner_name(tracee,st,path))return -ENAMETOOLONG;
    if(snprintf(directory,sizeof(directory),"%s/.pocket-owner",get_root(tracee))>=(int)sizeof(directory))return -ENAMETOOLONG;
    if(mkdir(directory,0700) && errno!=EEXIST)return -errno;
    struct stat dir;
    if(lstat(directory,&dir) || !S_ISDIR(dir.st_mode) || dir.st_uid!=getuid())return -EPERM;
    if(snprintf(temporary,sizeof(temporary),"%s.%d.tmp",path,getpid())>=(int)sizeof(temporary))return -ENAMETOOLONG;
    int fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return -errno;
    int status=write(fd,&owner,sizeof(owner))==sizeof(owner)?0:-EIO;
    close(fd);
    if(!status && rename(temporary,path))status=-errno;
    if(status)unlink(temporary);
    return status;
}

// PRoot has already translated pathname arguments when this helper runs.
static int pocket_owner_target(const Tracee* tracee,word_t nr,char* path,int* flags,RegVersion version) {
    int fd=AT_FDCWD;
    Reg argument=SYSARG_1;
    *flags=0;
    if(nr==PR_fchown || nr==PR_fchown32) {
        fd=(int)peek_reg(tracee,ORIGINAL,SYSARG_1);
        return snprintf(path,PATH_MAX,"/proc/%d/fd/%d",tracee->pid,fd)>=PATH_MAX?-ENAMETOOLONG:0;
    }
    if(nr==PR_fchownat || nr==PR_openat || nr==PR_mkdirat || nr==PR_mknodat) {
        fd=(int)peek_reg(tracee,ORIGINAL,SYSARG_1);argument=SYSARG_2;
    }
    if(nr==PR_symlink)argument=SYSARG_2;
    if(nr==PR_symlinkat) {fd=(int)peek_reg(tracee,ORIGINAL,SYSARG_2);argument=SYSARG_3;}
    if(nr==PR_lchown || nr==PR_lchown32 || nr==PR_symlink || nr==PR_symlinkat)*flags=AT_SYMLINK_NOFOLLOW;
    if(nr==PR_fchownat)*flags=(int)peek_reg(tracee,ORIGINAL,SYSARG_5)&AT_SYMLINK_NOFOLLOW;
    char translated[PATH_MAX];
    int status=read_path(tracee,translated,peek_reg(tracee,version,argument));
    if(status<0)return status;
    if(translated[0]=='/') {strcpy(path,translated);return 0;}
    int n=fd==AT_FDCWD?snprintf(path,PATH_MAX,"/proc/%d/cwd/%s",tracee->pid,translated):
        snprintf(path,PATH_MAX,"/proc/%d/fd/%d/%s",tracee->pid,fd,translated);
    if(!translated[0] && nr==PR_fchownat && ((int)peek_reg(tracee,ORIGINAL,SYSARG_5)&AT_EMPTY_PATH)) {
        *flags=0;n=snprintf(path,PATH_MAX,"/proc/%d/fd/%d",tracee->pid,fd);
    }
    return n<0 || n>=PATH_MAX?-ENAMETOOLONG:0;
}

static bool pocket_owner_inside(const Tracee* tracee,const char* path,int flags) {
    char actual[PATH_MAX],parent[PATH_MAX];
    const char* checked=path;
    // For lchown/symlink creation, resolve the containing directory only.
    if(flags&AT_SYMLINK_NOFOLLOW) {
        if(strlen(path)>=sizeof(parent))return false;
        strcpy(parent,path);char* slash=strrchr(parent,'/');
        if(!slash)return false;
        *slash=0;checked=parent;
    }
    if(!realpath(checked,actual))return false;
    size_t size=strlen(get_root(tracee));
    return !strncmp(actual,get_root(tracee),size) && (!actual[size] || actual[size]=='/');
}

static void pocket_owner_enter(Tracee* tracee,Config* config) {
    config->pocket_create=0;
    if(!pocket_owner_enabled(tracee))return;
    word_t nr=get_sysnum(tracee,ORIGINAL);
    if(nr==PR_timerfd_create) {
        long clock=(long)peek_reg(tracee,ORIGINAL,SYSARG_1);
        // Timers expire while the app runs; no host wake-alarm capability.
        if(clock==8)poke_reg(tracee,SYSARG_1,0);
        if(clock==9)poke_reg(tracee,SYSARG_1,7);
    }
    if(nr==PR_setgroups || nr==PR_setgroups32 || nr==PR_getgroups || nr==PR_getgroups32 || nr==PR_settimeofday)
        set_sysnum(tracee,PR_void);
    int create=(nr==PR_open && (peek_reg(tracee,ORIGINAL,SYSARG_2)&O_CREAT)) ||
        (nr==PR_openat && (peek_reg(tracee,ORIGINAL,SYSARG_3)&O_CREAT)) || nr==PR_creat;
    if(!create)return;
    char path[PATH_MAX];int flags;struct stat st;
    // MODIFIED is saved only after SYSCALL_ENTER_END callbacks return.
    if(!pocket_owner_target(tracee,nr,path,&flags,CURRENT) && lstat(path,&st) && errno==ENOENT)config->pocket_create=1;
}

// Return 1 when this extension supplied the final syscall result/metadata.
static int pocket_owner_exit(Tracee* tracee,Config* config) {
    if(!pocket_owner_enabled(tracee))return 0;
    word_t nr=get_sysnum(tracee,ORIGINAL);
    long result=(long)peek_reg(tracee,CURRENT,SYSARG_RESULT);
    // Android must see the app sandbox's actual capability mask. Logical
    // root ownership/UID emulation does not grant host kernel capabilities.
    if(nr==PR_capget)return 1;
    if(nr==PR_settimeofday) {poke_reg(tracee,SYSARG_RESULT,(word_t)-EPERM);return 1;}
    if(nr==PR_setgroups || nr==PR_setgroups32) {
        long count=(long)peek_reg(tracee,ORIGINAL,SYSARG_1);
        int status=config->euid!=0?-EPERM:(count<0 || count>64?-EINVAL:0);
        gid_t groups[64];
        if(!status && count && read_data(tracee,groups,peek_reg(tracee,ORIGINAL,SYSARG_2),count*sizeof(gid_t)))status=-EFAULT;
        if(!status) {memcpy(config->pocket_groups,groups,count*sizeof(gid_t));config->pocket_group_count=(unsigned)count;}
        poke_reg(tracee,SYSARG_RESULT,status);return 1;
    }
    if(nr==PR_getgroups || nr==PR_getgroups32) {
        long count=(long)peek_reg(tracee,ORIGINAL,SYSARG_1);
        long status=count<0 || (count>0 && count<config->pocket_group_count)?-EINVAL:(long)config->pocket_group_count;
        if(status>=0 && count && config->pocket_group_count &&
            write_data(tracee,peek_reg(tracee,ORIGINAL,SYSARG_2),config->pocket_groups,config->pocket_group_count*sizeof(gid_t)))status=-EFAULT;
        poke_reg(tracee,SYSARG_RESULT,status);return 1;
    }
    int at_stat=nr==PR_newfstatat || nr==PR_fstatat64;
    int basic_stat=nr==PR_fstat || nr==PR_fstat64 || nr==PR_stat || nr==PR_stat64 || nr==PR_lstat || nr==PR_lstat64;
    if((at_stat || basic_stat) && result==0) {
        word_t address=peek_reg(tracee,ORIGINAL,at_stat?SYSARG_3:SYSARG_2);
        struct stat st;
        if(read_data(tracee,&st,address,sizeof(st)) || st.st_uid!=getuid())return 0;
        PocketOwner owner=pocket_owner_read(tracee,&st);
        poke_uint32(tracee,address+offsetof_stat_uid(tracee),owner.uid);
        poke_uint32(tracee,address+offsetof_stat_gid(tracee),owner.gid);
        return 1;
    }
    int chown_call=nr==PR_chown || nr==PR_chown32 || nr==PR_lchown || nr==PR_lchown32 ||
        nr==PR_fchown || nr==PR_fchown32 || nr==PR_fchownat;
    int created=(config->pocket_create && result>=0) || (result==0 &&
        (nr==PR_mkdir || nr==PR_mkdirat || nr==PR_symlink || nr==PR_symlinkat || nr==PR_mknod || nr==PR_mknodat));
    if(!created && (!chown_call || (result!=0 && result!=-EPERM)))return 0;
    char path[PATH_MAX];int flags;struct stat st;
    if(config->pocket_create && result>=0) {
        flags=0;snprintf(path,sizeof(path),"/proc/%d/fd/%ld",tracee->pid,result);
    } else if(pocket_owner_target(tracee,nr,path,&flags,MODIFIED))return 0;
    if(!pocket_owner_inside(tracee,path,flags) || fstatat(AT_FDCWD,path,&st,flags))return 0;
    PocketOwner owner=created?(PocketOwner){0x504f434b,config->euid,config->egid}:pocket_owner_read(tracee,&st);
    if(chown_call) {
        Reg uid_arg=nr==PR_fchownat?SYSARG_3:SYSARG_2;
        Reg gid_arg=nr==PR_fchownat?SYSARG_4:SYSARG_3;
        uid_t uid=(uid_t)peek_reg(tracee,ORIGINAL,uid_arg);
        gid_t gid=(gid_t)peek_reg(tracee,ORIGINAL,gid_arg);
        if(config->euid!=0 && ((uid!=(uid_t)-1 && uid!=owner.uid) ||
            (gid!=(gid_t)-1 && gid!=config->egid) || config->euid!=owner.uid)) {
            poke_reg(tracee,SYSARG_RESULT,(word_t)-EPERM);return 1;
        }
        if(uid!=(uid_t)-1)owner.uid=uid;
        if(gid!=(gid_t)-1)owner.gid=gid;
    }
    int status=pocket_owner_write(tracee,&st,owner);
    if(chown_call || status)poke_reg(tracee,SYSARG_RESULT,status);
    return chown_call || status;
}
