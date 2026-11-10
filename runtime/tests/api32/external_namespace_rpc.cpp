// SPDX-License-Identifier: GPL-3.0-only
#include "fuse_namespace_peer.h"
#include "../../api32/external_open_rpc.h"
#include <sys/wait.h>
#include <cstdlib>
extern "C" int external_namespace_fixture_init(const char*,unsigned);
extern "C" long external_namespace_fixture_fd(long,int,uintptr_t);
extern "C" long external_namespace_fixture_path(long,const uint64_t[6],const char*,const char*,const char*);
extern "C" long external_namespace_fixture_umask();
extern "C" void external_namespace_fixture_mode(unsigned);
extern "C" long external_namespace_fixture_directory(long,int,uintptr_t,uint64_t);
static unsigned descriptorCount() {
    DIR* d=opendir("/proc/self/fd");assert(d);unsigned n=0;while(readdir(d))n++;closedir(d);return n;
}
static int connectTo(const std::string& path) {
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);assert(fd>=0);sockaddr_un address{};address.sun_family=AF_UNIX;
    assert(path.size()<sizeof(address.sun_path));strcpy(address.sun_path,path.c_str());assert(!connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)));return fd;
}
template<class Reply=PocketExternalNamespaceReply>
static Reply request(const std::string& socket,unsigned op,const std::string& path="",const std::string& second="",unsigned flags=0,uint64_t length=0,int proof=-1,unsigned rights=1,unsigned permissions=0,unsigned mask=0,pid_t tid=getpid(),unsigned bases=0) {
    int fd=connectTo(socket);PocketExternalRequest2 req{{POCKET_EXTERNAL_OPEN_MAGIC,2,flags,static_cast<uint32_t>(tid),static_cast<uint32_t>(path.size()),op,{static_cast<uint32_t>(second.size()),bases}},permissions,mask,length};
    // Send a partial header with ancillary data, then its remainder. The
    // server must retain/close the received rights across partial reads.
    char control[CMSG_SPACE(9*sizeof(int))]{};iovec part{&req,1};msghdr message{};message.msg_iov=&part;message.msg_iovlen=1;
    if(proof>=0){assert(rights>=1 && rights<=9);message.msg_control=control;message.msg_controllen=CMSG_SPACE(rights*sizeof(int));cmsghdr* c=CMSG_FIRSTHDR(&message);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(rights*sizeof(int));for(unsigned i=0;i<rights;i++)memcpy(reinterpret_cast<char*>(CMSG_DATA(c))+i*sizeof(int),&proof,sizeof(proof));}
    assert(sendmsg(fd,&message,MSG_NOSIGNAL)==1);
    std::string tail(reinterpret_cast<char*>(&req)+1,sizeof(req)-1);tail+=path;tail+=second;
    // MSG_CTRUNC is rejected before the remaining frame is read. Queueing
    // these few bytes may race that close, so an early EPIPE is expected.
    auto n=send(fd,tail.data(),tail.size(),MSG_NOSIGNAL);assert(n==static_cast<ssize_t>(tail.size()) || (n<0 && (errno==EPIPE || errno==ECONNRESET)));
    Reply reply{};size_t at=0;
    while(at<sizeof(reply)){
        n=recv(fd,reinterpret_cast<char*>(&reply)+at,sizeof(reply)-at,0);
        // Excess rights can truncate the very first one-byte header read.
        // Version/opcode are then unknown: the broker returns the common
        // open-error prefix, rather than guessing a namespace reply format.
        if(rights>8 && at==sizeof(PocketExternalReply) && reply.error==-EPROTO &&
            (n==0 || (n<0 && errno==ECONNRESET)))break;
        assert(n>0);at+=n;
    }
    assert(!reply.reserved);close(fd);return reply;
}
#ifdef POCKET_DIRECTORY_RPC32
static int directoryLibcClient() {
    char status[4096];ssize_t n=readIdentityFile("/proc/self/stat",status,sizeof(status));assert(n>0);
    PocketBinderIdentity record{POCKET_IDENTITY_MAGIC,static_cast<uint32_t>(getpid()),10001,10002,pocket_process_start(status,n)};assert(record.start_ticks);
    std::string path="/identity/"+std::to_string(getpid());int output=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);assert(output>=0 && write(output,&record,sizeof(record))==sizeof(record));close(output);
    assert(!setgid(10002) && !setuid(10001) && getuid()==10001 && getgid()==10002);
    errno=0;assert(syscall(__NR_getpid,0x504c2001UL,8UL,1UL)==-1 && errno==EPERM);
    const char* directory="/storage/emulated/0/a";DIR* stream=opendir(directory);assert(stream);int fd=dirfd(stream);struct stat held{},named{};
    assert(!fstat(fd,&held) && S_ISDIR(held.st_mode) && !stat(directory,&named) && held.st_dev==named.st_dev && held.st_ino==named.st_ino);
    auto list=[&] {std::vector<std::string> names;errno=0;while(auto entry=readdir(stream))if(strcmp(entry->d_name,".") && strcmp(entry->d_name,".."))names.push_back(entry->d_name);assert(!errno);std::sort(names.begin(),names.end());return names;};
    assert((list()==std::vector<std::string>{"file","second"}));rewinddir(stream);assert((list()==std::vector<std::string>{"file","second"}));
    mode_t oldMask=umask(0027);assert(!mkdirat(fd,"relative",0777));
    int relative=openat(fd,"relative",O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(relative>=0);
    struct stat relativeStat{};assert(!fstat(relative,&relativeStat) && S_ISDIR(relativeStat.st_mode));
    assert(!rename("/storage/emulated/0/a","/storage/emulated/0/moved"));
    assert(!fstatat(fd,"file",&named,0) && S_ISREG(named.st_mode));
    assert(!renameat(fd,"relative",fd,"renamed"));assert(!fstatat(relative,".",&named,0) && named.st_ino==relativeStat.st_ino);
    assert(!mkdirat(relative,"nested",0777));int nested=openat(relative,"nested",O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(nested>=0);assert(!close(relative));
    assert(!unlinkat(fd,"renamed/nested",AT_REMOVEDIR));assert(!fstatat(nested,".",&named,0) && !named.st_nlink);
    errno=0;assert(mkdirat(nested,"gone",0777)==-1 && errno==ENOENT);assert(!close(nested));
    assert(!unlinkat(fd,"renamed",AT_REMOVEDIR));assert(!rename("/storage/emulated/0/moved","/storage/emulated/0/a"));umask(oldMask);
    assert(!fsync(fd));char bytes[32]{};errno=0;assert(read(fd,bytes,1)==-1 && errno==EISDIR);
    sockaddr_un address{};socklen_t length=sizeof(address);errno=0;assert(getsockname(fd,reinterpret_cast<sockaddr*>(&address),&length)==-1 && errno==ENOTSOCK);
    assert((fcntl(fd,F_GETFL)&O_ACCMODE)==O_RDONLY && (fcntl(fd,F_GETFD)&FD_CLOEXEC));assert(!closedir(stream));
    assert(!unlink(path.c_str()));
    puts("DIRECTORY_LIBC_CLIENT_PASS uid=10001 kernel_scope_denied=1 real_opendir=1 real_readdir=1 real_rewinddir=1 path_fd_metadata=1 real_sync=1 read_eisdir=1 socket_facade_denied=1 close=1 relative_open_stat_mkdir_rename_remove=1 renamed_parent_fd=1 unlinked_directory_fd=1");return 0;
}
#ifdef __ANDROID__
static void directoryLibcBridge(const std::string& base) {
    const char* libraries=getenv("LD_LIBRARY_PATH");const char* run=getenv("POCKET_TEST_RUN");assert(libraries && run && strlen(run)==32);
    std::string root=libraries,suffix="/system/lib64";assert(root.size()>suffix.size() && root.compare(root.size()-suffix.size(),suffix.size(),suffix)==0);root.resize(root.size()-suffix.size());
    std::string target=root+base;
    // This directory-only child has no kernel AppFuse proxy mount. The parent
    // component fixture uses ordinary files as FD-proof surrogates, whose
    // device also contains Bionic. Give the child an explicitly synthetic
    // logical volume device so it cannot mistake its libraries for proxies.
    uint64_t originalDevice=0;std::string devicePath=base+"/.pocket-external-device";
    assert(readIdentityFile(devicePath.c_str(),&originalDevice,sizeof(originalDevice))==sizeof(originalDevice));
    auto writeDevice=[&](uint64_t value){int fd=open(devicePath.c_str(),O_WRONLY|O_TRUNC|O_CLOEXEC);assert(fd>=0 && write(fd,&value,sizeof(value))==sizeof(value));assert(!close(fd));};
    writeDevice(originalDevice^0x4000000000000000ULL);
    for(const auto& path:{base+"/system",base+"/system/bin",base+"/system/lib64"})assert(!mkdir(path.c_str(),0700));
    auto copy=[&](const std::string& path){int input=open(("/system/"+path).c_str(),O_RDONLY|O_CLOEXEC),output=open((base+"/system/"+path).c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0755);assert(input>=0 && output>=0);
        char bytes[8192];for(;;){ssize_t n=read(input,bytes,sizeof(bytes));assert(n>=0);if(!n)break;size_t at=0;while(at<static_cast<size_t>(n)){ssize_t wrote=write(output,bytes+at,n-at);assert(wrote>0);at+=wrote;}}close(input);assert(!close(output));};
    for(const char* name:{"pocket-loader","linker64","fuse-stock"})copy(std::string("bin/")+name);
    for(const char* name:{"libc.so","libdl.so","ld-android.so","libm.so","libstdc++.so","libfuse.so"})copy(std::string("lib64/")+name);
    std::string ready="{\"runId\":\""+std::string(run)+"\",\"root\":\""+target+"\"}\n";
    std::string prefix="/tmp/directory-child-"+std::string(run),readyPath=prefix+"-ready.json",donePath=prefix+"-done",next=prefix+"-ready.new";
    int output=open(next.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);assert(output>=0 && write(output,ready.data(),ready.size())==static_cast<ssize_t>(ready.size()));close(output);
    assert(!rename(next.c_str(),readyPath.c_str()));
    bool complete=false;
    for(unsigned i=0;i<2000;i++){char result[16]{};ssize_t n=readIdentityFile(donePath.c_str(),result,sizeof(result));if(n>0){assert(n==2 && !memcmp(result,"0\n",2));complete=true;break;}usleep(20000);}
    assert(complete);writeDevice(originalDevice);assert(!unlink(readyPath.c_str()) && !unlink(donePath.c_str()));
    for(const char* name:{"linker64","fuse-stock","pocket-loader"})assert(!unlink((base+"/system/bin/"+name).c_str()));
    for(const char* name:{"libc.so","libdl.so","ld-android.so","libm.so","libstdc++.so","libfuse.so"})assert(!unlink((base+"/system/lib64/"+name).c_str()));
    assert(!rmdir((base+"/system/lib64").c_str()) && !rmdir((base+"/system/bin").c_str()) && !rmdir((base+"/system").c_str()));
    // The host prepares this child root's owner directory for its interpreter.
    std::string owner=base+"/.pocket-owner";DIR* directory=opendir(owner.c_str());assert(directory);while(auto item=readdir(directory))if(strcmp(item->d_name,".") && strcmp(item->d_name,".."))assert(!unlink((owner+"/"+item->d_name).c_str()));closedir(directory);assert(!rmdir(owner.c_str()));
    std::string identity=base+"/.pocket-identity";directory=opendir(identity.c_str());assert(directory);while(auto item=readdir(directory))if(strcmp(item->d_name,".") && strcmp(item->d_name,".."))assert(!unlink((identity+"/"+item->d_name).c_str()));closedir(directory);assert(!rmdir(identity.c_str()));
    puts("DIRECTORY_LIBC_BRIDGE_PASS separate_host_process=1 fresh_interpreter_filter=1 ordinary_guest_uid=10001 synthetic_directory_device=1 no_appfuse_mount=1");
}
#endif
static std::string directoryName(const char* bytes,size_t size,uint64_t* inode=nullptr) {
    assert(size>=24 && !(size&7));uint16_t length=0;memcpy(&length,bytes+16,2);assert(length==size && bytes[size-1]==0);
    if(inode)memcpy(inode,bytes,8);
    return std::string(bytes+19);
}
static void reopenTypedProof(Files& files,Identity actor,const std::string& lower) {
    auto before=files.count();std::vector<Files::Opened> caps;
    auto opened=[&](const char* path,int flags){Files::Opened c{};assert(!files.open(path,flags,actor,&c,0666,0027));caps.push_back(c);return c;};
    auto reopened=[&](const Files::Opened& from,int flags,Identity caller){Files::Opened c{};assert(!files.reopen(from,flags,caller,&c));caps.push_back(c);return c;};
    assert(!files.mkdir("/0/reopen",0777,0027,actor));
    auto file=opened("/0/reopen/file",O_RDWR|O_CREAT|O_EXCL),metadata=opened("/0/reopen/file",O_PATH);
    auto dir=opened("/0/reopen",O_RDONLY|O_DIRECTORY),pathDir=opened("/0/reopen",O_PATH|O_DIRECTORY);
    uint32_t written=0;assert(!files.write(file.slot,file.cookie,0,{'o','r','i','g','i','n','a','l'},actor,&written) && written==8);
    auto fresh=reopened(metadata,O_RDONLY,actor);std::vector<char> bytes;
    assert(!files.read(fresh.slot,fresh.cookie,0,8,actor,&bytes) && std::string(bytes.begin(),bytes.end())=="original");
    auto changed=actor;changed.uid=33333;Files::Opened rejected{};
    assert(files.reopen(metadata,O_RDONLY,changed,&rejected)==-EACCES);
    auto grantedMetadata=reopened(metadata,O_PATH,changed);fuse_attr st{};
    assert(!files.attributes(grantedMetadata.slot,grantedMetadata.cookie,changed,&st));auto inode=st.ino;
    assert(files.read(grantedMetadata.slot,grantedMetadata.cookie,0,1,changed,&bytes)==-EBADF);
    assert(files.reopen(grantedMetadata,O_RDONLY,changed,&rejected)==-EACCES);
    assert(!files.rename("/0/reopen","/0/reopen-moved",actor));
    fresh=reopened(file,O_RDONLY,actor);assert(!files.attributes(fresh.slot,fresh.cookie,actor,&st) && st.ino==inode);
    int witness=open((lower+"/0/reopen-moved/file").c_str(),O_RDONLY|O_CLOEXEC);assert(witness>=0);
    assert(!files.remove("/0/reopen-moved/file",false,actor));
    auto replacement=opened("/0/reopen-moved/file",O_RDWR|O_CREAT|O_EXCL);
    assert(!files.write(replacement.slot,replacement.cookie,0,{'n','e','w'},actor,&written));
    fresh=reopened(metadata,O_RDONLY,actor);assert(!files.attributes(fresh.slot,fresh.cookie,actor,&st) && st.ino==inode && !st.nlink);
    assert(!files.read(fresh.slot,fresh.cookie,0,8,actor,&bytes) && std::string(bytes.begin(),bytes.end())=="original");
    assert(files.reopen(metadata,O_RDONLY,changed,&rejected)==-EACCES);
    assert(files.reopen(metadata,O_RDONLY|O_DIRECTORY,actor,&rejected)==-ENOTDIR);
    assert(!fchmod(witness,0));assert(files.reopen(metadata,O_RDONLY,actor,&rejected)==-EACCES);
    assert(!files.read(file.slot,file.cookie,0,8,actor,&bytes) && bytes.size()==8);assert(!fchmod(witness,0640));close(witness);
    assert(!files.remove("/0/reopen-moved/file",false,actor));assert(!files.remove("/0/reopen-moved",true,actor));
    for(const auto& from:{dir,pathDir}) {
        auto c=reopened(from,O_RDONLY|O_DIRECTORY,actor);std::vector<Files::DirectoryEntry> entries;
        assert(!files.list(c.slot,c.cookie,0,4096,actor,&entries) && entries.empty());
        assert(!files.attributes(c.slot,c.cookie,actor,&st) && S_ISDIR(st.mode) && !st.nlink);
        assert(files.reopen(from,O_RDONLY|O_DIRECTORY,changed,&rejected)==-EACCES);
    }
    for(auto c:caps)assert(!files.release(c.slot,c.cookie,actor));
    assert(files.count()==before);
    puts("REOPEN_TYPED_PASS live_current_uid=1 metadata_not_data_grant=1 renamed_inode=1 unlinked_file_data=1 replacement_not_opened=1 current_execute_read_permissions=1 unlinked_directory_empty=1 handles_references_balanced=1");
}
static void directoryProof(Files& files,const std::string& path,const std::string& mapped,Identity actor,uint64_t fileInode,const std::string& base,Peer& peer) {
    auto openDirectory=[&]{uint64_t args[6]={static_cast<uint64_t>(AT_FDCWD),0,O_RDONLY|O_DIRECTORY|O_CLOEXEC,0,0,0};long fd=external_namespace_fixture_path(SYS_openat,args,path.c_str(),mapped.c_str(),"");assert(fd>=0);return static_cast<int>(fd);};
    int fd=openDirectory(),duplicate=fcntl(fd,F_DUPFD_CLOEXEC,0);assert(duplicate>=0 && (fcntl(fd,F_GETFD)&FD_CLOEXEC));
    auto reopenFD=[&](int source,int flags){std::string exact="/proc/self/fd/"+std::to_string(source);uint64_t args[6]={static_cast<uint64_t>(AT_FDCWD),0,static_cast<uint64_t>(flags),0,0,0};return external_namespace_fixture_path(SYS_openat,args,exact.c_str(),exact.c_str(),"");};
    struct stat st{};assert(!external_namespace_fixture_fd(SYS_fstat,fd,reinterpret_cast<uintptr_t>(&st)) && S_ISDIR(st.st_mode));
    int savedCwd=open(".",O_PATH|O_DIRECTORY|O_CLOEXEC);assert(savedCwd>=0);
    mode_t savedMode=st.st_mode&07777;assert(!chmod(mapped.c_str(),0600));
    assert(external_namespace_fixture_fd(SYS_fchdir,duplicate,0)==-EACCES);
    assert(!chmod(mapped.c_str(),savedMode));
    assert(!external_namespace_fixture_fd(SYS_fchdir,duplicate,0));
    char cwd[4096];assert(getcwd(cwd,sizeof(cwd)) && mapped==cwd);
    struct stat cwdStat{};assert(!stat(".",&cwdStat) && cwdStat.st_ino==st.st_ino);
    // Kernel cwd owns its reference independently of all descriptor copies.
    pid_t cwdChild=fork();assert(cwdChild>=0);
    if(!cwdChild) {
        close(fd);close(duplicate);struct stat inherited{};
        _exit(stat("file",&inherited)==0 && inherited.st_ino==fileInode?0:1);
    }
    int cwdStatus=0;assert(waitpid(cwdChild,&cwdStatus,0)==cwdChild && WIFEXITED(cwdStatus) && !WEXITSTATUS(cwdStatus));
    assert(!fchdir(savedCwd));
    auto procChild=[&](const std::string& prefix,int cap,const std::string& child){return prefix+std::to_string(cap)+"/"+child;};
    for(const auto& prefix:{std::string("/proc/self/fd/"),std::string("/proc/thread-self/fd/"),"/proc/"+std::to_string(getpid())+"/fd/"}) {
        auto exact=prefix+std::to_string(duplicate);struct stat exactStat{};
        uint64_t exactArgs[6]={static_cast<uint64_t>(AT_FDCWD),0,reinterpret_cast<uintptr_t>(&exactStat),0,0,0};
        assert(!external_namespace_fixture_path(SYS_newfstatat,exactArgs,exact.c_str(),exact.c_str(),"") && S_ISDIR(exactStat.st_mode) && exactStat.st_ino==st.st_ino);
        exactArgs[2]=1;assert(external_namespace_fixture_path(SYS_newfstatat,exactArgs,exact.c_str(),exact.c_str(),"")==-EFAULT);
        exactArgs[2]=reinterpret_cast<uintptr_t>(&exactStat);exactArgs[3]=AT_REMOVEDIR;
        assert(external_namespace_fixture_path(SYS_newfstatat,exactArgs,exact.c_str(),exact.c_str(),"")==-EINVAL);
        exactArgs[3]=AT_SYMLINK_NOFOLLOW;
        assert(external_namespace_fixture_path(SYS_newfstatat,exactArgs,exact.c_str(),exact.c_str(),"")==-ENOMSG);
        assert(!lstat(exact.c_str(),&exactStat) && S_ISLNK(exactStat.st_mode));
        assert(!external_namespace_fixture_path(SYS_chdir,exactArgs,exact.c_str(),exact.c_str(),""));
        assert(getcwd(cwd,sizeof(cwd)) && mapped==cwd);assert(!fchdir(savedCwd));
        auto alias=procChild(prefix,duplicate,"file");struct stat through{};
        int raw=open(alias.c_str(),O_PATH|O_CLOEXEC);assert(raw<0 && errno==ENOTDIR);
        uint64_t args[6]={static_cast<uint64_t>(AT_FDCWD),0,reinterpret_cast<uintptr_t>(&through),0,0,0};
        assert(!external_namespace_fixture_path(SYS_newfstatat,args,alias.c_str(),alias.c_str(),"") && through.st_ino==fileInode);
        args[2]=O_PATH|O_CLOEXEC;
        long metadata=external_namespace_fixture_path(SYS_openat,args,alias.c_str(),alias.c_str(),"");assert(metadata>=0);
        assert(!external_namespace_fixture_fd(SYS_fstat,metadata,reinterpret_cast<uintptr_t>(&through)) && through.st_ino==fileInode);
        close(metadata);
        char target[4096];uint64_t readlinkArgs[6]={static_cast<uint64_t>(AT_FDCWD),0,reinterpret_cast<uintptr_t>(target),sizeof(target),0,0};
        assert(external_namespace_fixture_path(SYS_readlinkat,readlinkArgs,alias.c_str(),alias.c_str(),"")==-EINVAL);
        auto absent=procChild(prefix,fd,"absent");
        assert(external_namespace_fixture_path(SYS_readlinkat,readlinkArgs,absent.c_str(),absent.c_str(),"")==-ENOENT);
    }
    std::thread procWorker([&]{
        auto alias=procChild("/proc/"+std::to_string(syscall(SYS_gettid))+"/fd/",fd,"file");
        struct stat through{};uint64_t args[6]={static_cast<uint64_t>(AT_FDCWD),0,reinterpret_cast<uintptr_t>(&through),0,0,0};
        assert(!external_namespace_fixture_path(SYS_newfstatat,args,alias.c_str(),alias.c_str(),"") && through.st_ino==fileInode);
    });procWorker.join();
    auto from=procChild("/proc/self/fd/",fd,"proc-child"),to=procChild("/proc/thread-self/fd/",duplicate,"proc-renamed");
    mode_t procMask=umask(0027);
    uint64_t create[6]={static_cast<uint64_t>(AT_FDCWD),0,0777,0,0,0};
    assert(!external_namespace_fixture_path(SYS_mkdirat,create,from.c_str(),from.c_str(),""));
    uint64_t move[6]={static_cast<uint64_t>(AT_FDCWD),0,static_cast<uint64_t>(AT_FDCWD),reinterpret_cast<uintptr_t>(to.c_str()),0,0};
    assert(!external_namespace_fixture_path(SYS_renameat,move,from.c_str(),from.c_str(),to.c_str()));
    uint64_t remove[6]={static_cast<uint64_t>(AT_FDCWD),0,AT_REMOVEDIR,0,0,0};
    assert(!external_namespace_fixture_path(SYS_unlinkat,remove,to.c_str(),to.c_str(),""));
    assert(umask(procMask)==0027);
    assert(!files.rename("/0/a","/0/proc-moved",actor));
    assert(!external_namespace_fixture_fd(SYS_fchdir,duplicate,0));
    assert(getcwd(cwd,sizeof(cwd)) && base+"/system-data/media/0/proc-moved"==cwd);
    int cwdFile=open("file",O_PATH|O_CLOEXEC);assert(cwdFile>=0 && !fstat(cwdFile,&cwdStat) && cwdStat.st_ino==fileInode);close(cwdFile);
    assert(!fchdir(savedCwd));
    auto held=procChild("/proc/self/fd/",fd,"file");struct stat renamed{};
    uint64_t lookup[6]={static_cast<uint64_t>(AT_FDCWD),0,reinterpret_cast<uintptr_t>(&renamed),0,0,0};
    assert(!external_namespace_fixture_path(SYS_newfstatat,lookup,held.c_str(),held.c_str(),"") && renamed.st_ino==fileInode);
    assert(!files.rename("/0/proc-moved","/0/a",actor));
    auto foreign=procChild("/proc/"+std::to_string(getppid())+"/fd/",fd,"file");
    assert(external_namespace_fixture_path(SYS_newfstatat,lookup,foreign.c_str(),foreign.c_str(),"")==-ENOMSG);
    struct statfs filesystem{},rawSocket{};
    assert(!fstatfs(fd,&rawSocket) && rawSocket.f_type!=0x65735546);
    assert(!external_namespace_fixture_fd(SYS_fstatfs,fd,reinterpret_cast<uintptr_t>(&filesystem)));
    assert(filesystem.f_type==0x65735546 && filesystem.f_bsize>0 && filesystem.f_blocks>0 &&
        filesystem.f_bavail<=filesystem.f_bfree && filesystem.f_bfree<=filesystem.f_blocks);
    assert(external_namespace_fixture_fd(SYS_fstatfs,duplicate,1)==-EFAULT);
    for(bool directory:{false,true}) {
        uint64_t args[6]={static_cast<uint64_t>(AT_FDCWD),0,static_cast<uint64_t>(O_PATH|O_CLOEXEC|(directory?O_DIRECTORY:0)),0,0,0};
        auto name=directory?path:path+"/file",lower=directory?mapped:mapped+"/file";
        long metadata=external_namespace_fixture_path(SYS_openat,args,name.c_str(),lower.c_str(),"");assert(metadata>=0);
        struct statfs result{};
        assert(!external_namespace_fixture_fd(SYS_fstatfs,metadata,reinterpret_cast<uintptr_t>(&result)));
        assert(result.f_type==filesystem.f_type && result.f_bsize==filesystem.f_bsize && result.f_blocks>0);
        assert(external_namespace_fixture_fd(SYS_fchdir,metadata,0)==(directory?0:-ENOTDIR));
        assert(!fchdir(savedCwd));
        assert(!close(metadata));
    }
    auto statsSocket=base+"/dev/socket/pocket_efs";
    auto invalid=request<PocketExternalStatfsReply>(statsSocket,POCKET_EXT_STATFS,"","",1,0,fd);
    assert(invalid.error==-EINVAL && !invalid.blocks && !invalid.bsize);
    invalid=request<PocketExternalStatfsReply>(statsSocket,POCKET_EXT_STATFS,"","",0,0,fd,2);
    assert(invalid.error==-EINVAL && !invalid.blocks && !invalid.bsize);
    int forged[2];assert(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,forged));
    invalid=request<PocketExternalStatfsReply>(statsSocket,POCKET_EXT_STATFS,"","",0,0,forged[0]);
    assert(invalid.error==-EBADF && !invalid.blocks && !invalid.bsize);close(forged[0]);close(forged[1]);
    auto identityPath=base+"/identity/"+std::to_string(getpid());PocketBinderIdentity identity{};
    assert(readIdentityFile(identityPath.c_str(),&identity,sizeof(identity))==sizeof(identity));
    auto replaceIdentity=[&]{int record=open(identityPath.c_str(),O_WRONLY|O_TRUNC|O_CLOEXEC);assert(record>=0 && write(record,&identity,sizeof(identity))==sizeof(identity));close(record);};
    identity.uid=33333;replaceIdentity();
    assert(external_namespace_fixture_fd(SYS_fstatfs,fd,reinterpret_cast<uintptr_t>(&filesystem))==-EACCES);
    assert(external_namespace_fixture_fd(SYS_fchdir,fd,0)==-EACCES);
    assert(reopenFD(fd,O_RDONLY|O_DIRECTORY)==-EACCES);
    long metadataOnly=reopenFD(fd,O_PATH|O_CLOEXEC);assert(metadataOnly>=0);
    assert(reopenFD(metadataOnly,O_RDONLY|O_DIRECTORY)==-EACCES);close(metadataOnly);
    struct stat beforeCwd{},afterCwd{};assert(!fstat(savedCwd,&beforeCwd) && !stat(".",&afterCwd) && beforeCwd.st_ino==afterCwd.st_ino && beforeCwd.st_dev==afterCwd.st_dev);
    identity.uid=actor.uid;replaceIdentity();
    auto malformedSearch=request<PocketExternalReply>(statsSocket,POCKET_EXT_DIR_SEARCH,"","",1,0,fd);assert(malformedSearch.error==-EINVAL);
    malformedSearch=request<PocketExternalReply>(statsSocket,POCKET_EXT_DIR_SEARCH,"","",0,0,fd,2);assert(malformedSearch.error==-EINVAL);
    int fakeSearch[2];assert(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,fakeSearch));
    malformedSearch=request<PocketExternalReply>(statsSocket,POCKET_EXT_DIR_SEARCH,"","",0,0,fakeSearch[0]);assert(malformedSearch.error==-EBADF);close(fakeSearch[0]);close(fakeSearch[1]);
    uint64_t chdirArgs[6]={0,0,0,0,0,0};assert(!external_namespace_fixture_path(SYS_chdir,chdirArgs,path.c_str(),mapped.c_str(),""));
    assert(getcwd(cwd,sizeof(cwd)) && mapped==cwd);assert(!fchdir(savedCwd));close(savedCwd);
    long flags=external_namespace_fixture_directory(SYS_fcntl,fd,F_GETFL,0);assert(flags>=0 && (flags&O_DIRECTORY) && (flags&O_ACCMODE)==O_RDONLY);
    assert(!external_namespace_fixture_directory(SYS_fcntl,fd,F_SETFL,flags|O_NONBLOCK));
    assert(external_namespace_fixture_directory(SYS_fcntl,duplicate,F_GETFL,0)&O_NONBLOCK);
    char bytes[65536]{};auto read=[&](int cap,char* buffer,unsigned size){return external_namespace_fixture_directory(SYS_getdents64,cap,reinterpret_cast<uintptr_t>(buffer),size);};
    assert(read(fd,reinterpret_cast<char*>(1),32)==-EFAULT);
    assert(read(fd,bytes,23)==-EINVAL);
    auto n=read(fd,bytes,32);uint64_t inode=0;assert(n>0 && directoryName(bytes,n,&inode)=="file" && inode==fileInode);
    long newDescription=reopenFD(fd,O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(newDescription>=0 && (fcntl(newDescription,F_GETFD)&FD_CLOEXEC));
    char freshBytes[32];assert(read(newDescription,freshBytes,sizeof(freshBytes))>0);close(newDescription);
    newDescription=reopenFD(fd,O_RDONLY|O_DIRECTORY);assert(newDescription>=0 && !(fcntl(newDescription,F_GETFD)&FD_CLOEXEC));close(newDescription);
    assert(read(duplicate,bytes,32)==0);assert(external_namespace_fixture_directory(SYS_lseek,duplicate,0,SEEK_CUR)>0);
    assert(!external_namespace_fixture_directory(SYS_lseek,duplicate,0,SEEK_SET));
    assert(read(fd,bytes,32)>0);assert(!external_namespace_fixture_fd(SYS_fsync,fd,0) && !external_namespace_fixture_fd(SYS_fdatasync,fd,0));
    assert(external_namespace_fixture_fd(SYS_read,fd,reinterpret_cast<uintptr_t>(bytes))==-EISDIR);
    assert(external_namespace_fixture_fd(SYS_getsockname,fd,0)==-ENOTSOCK);
    assert(external_namespace_fixture_fd(SYS_ioctl,fd,0)==-ENOTTY);
    assert(external_namespace_fixture_directory(SYS_lseek,fd,static_cast<uintptr_t>(-1),SEEK_SET)==-EINVAL);
    Files::Opened second{};assert(!files.open("/0/a/second",O_CREAT|O_EXCL|O_RDWR,actor,&second,0666,0027));assert(!files.release(second.slot,second.cookie,actor));
    assert(!external_namespace_fixture_directory(SYS_lseek,fd,0,SEEK_SET));
    char workerBytes[32]{};long workerSize=-1;
    std::thread worker([&]{workerSize=read(duplicate,workerBytes,sizeof(workerBytes));});
    auto mainSize=read(fd,bytes,32);worker.join();assert(mainSize>0 && workerSize>0);
    std::vector<std::string> names={directoryName(bytes,mainSize),directoryName(workerBytes,workerSize)};std::sort(names.begin(),names.end());assert((names==std::vector<std::string>{"file","second"}));
    assert(read(fd,bytes,32)==0);
    // SCM_RIGHTS proves the held directory itself. These names are never
    // resolved through the kernel carrier or its /proc socket link.
    mode_t relativeMask=umask(0027);
    uint64_t relativeArgs[6]={static_cast<uint64_t>(fd),0,0777,0,0,0};
    assert(!external_namespace_fixture_path(SYS_mkdirat,relativeArgs,"relative","relative",""));
    relativeArgs[2]=reinterpret_cast<uintptr_t>(&st);
    assert(!external_namespace_fixture_path(SYS_newfstatat,relativeArgs,"relative","relative","") && S_ISDIR(st.st_mode));
    relativeArgs[2]=O_RDONLY|O_DIRECTORY|O_CLOEXEC;
    long relative=external_namespace_fixture_path(SYS_openat,relativeArgs,"relative","relative","");assert(relative>=0);
    relativeArgs[2]=O_PATH|O_DIRECTORY|O_CLOEXEC;
    long relativePath=external_namespace_fixture_path(SYS_openat,relativeArgs,"relative","relative","");assert(relativePath>=0);
    struct stat unlinkedDirectory{};assert(!external_namespace_fixture_fd(SYS_fstat,relative,reinterpret_cast<uintptr_t>(&unlinkedDirectory)));
    uint64_t renameArgs[6]={static_cast<uint64_t>(fd),0,static_cast<uint64_t>(fd),reinterpret_cast<uintptr_t>("renamed"),0,0};
    assert(!external_namespace_fixture_path(SYS_renameat,renameArgs,"relative","relative","renamed"));
    relativeArgs[2]=AT_REMOVEDIR;assert(!external_namespace_fixture_path(SYS_unlinkat,relativeArgs,"renamed","renamed",""));
    assert(!external_namespace_fixture_fd(SYS_fstat,relative,reinterpret_cast<uintptr_t>(&st)) && !st.st_nlink);
    // Deleted path GETATTR is unavailable; search must use the authenticated
    // held provider node, with fresh caller permission rather than an old name.
    peer.staleGetattrInode=unlinkedDirectory.st_ino;
    int originalCwd=open(".",O_PATH|O_DIRECTORY|O_CLOEXEC);assert(originalCwd>=0);
    for(int heldDirectory:{static_cast<int>(relative),static_cast<int>(relativePath)}) {
        assert(!external_namespace_fixture_fd(SYS_fchdir,heldDirectory,0));
        char missing[4096];errno=0;assert(!getcwd(missing,sizeof(missing)) && errno==ENOENT);
        assert(!fchdir(originalCwd));
        std::string exact="/proc/self/fd/"+std::to_string(heldDirectory);struct stat followed{};
        uint64_t args[6]={static_cast<uint64_t>(AT_FDCWD),0,reinterpret_cast<uintptr_t>(&followed),0,0,0};
        assert(!external_namespace_fixture_path(SYS_newfstatat,args,exact.c_str(),exact.c_str(),"") && S_ISDIR(followed.st_mode) && !followed.st_nlink && followed.st_ino==unlinkedDirectory.st_ino);
        assert(!external_namespace_fixture_path(SYS_chdir,args,exact.c_str(),exact.c_str(),""));
        errno=0;assert(!getcwd(missing,sizeof(missing)) && errno==ENOENT);assert(!fchdir(originalCwd));
    }
    identity.uid=33333;replaceIdentity();assert(external_namespace_fixture_fd(SYS_fchdir,relativePath,0)==-EACCES);
    identity.uid=actor.uid;replaceIdentity();
    peer.staleGetattrInode=0;close(originalCwd);close(relativePath);close(relative);
    auto socket=base+"/dev/socket/pocket_efs";
    assert(request<PocketExternalReply>(socket,POCKET_EXT_FD_REOPEN,"","",0,0,fd,2).error==-EINVAL);
    assert(request<PocketExternalReply>(socket,POCKET_EXT_FD_REOPEN,"","",0,0,fd,1,1).error==-EINVAL);
    assert(request<PocketExternalReply>(socket,POCKET_EXT_FD_REOPEN,"","",0x80000000U,0,fd).error==-EINVAL);
    assert(request(socket,POCKET_EXT_STAT,"file","",0,0,-1,1,0,0,getpid(),POCKET_EXTERNAL_AT_FIRST).error==-EINVAL);
    assert(request(socket,POCKET_EXT_STAT,"file","",0,0,fd,2,0,0,getpid(),POCKET_EXTERNAL_AT_FIRST).error==-EINVAL);
    assert(request(socket,POCKET_EXT_STAT,"/absolute","",0,0,fd,1,0,0,getpid(),POCKET_EXTERNAL_AT_FIRST).error==-EINVAL);
    assert(request(socket,POCKET_EXT_STAT,"../escape","",0,0,fd,1,0,0,getpid(),POCKET_EXTERNAL_AT_FIRST).error==-ENOENT);
    assert(umask(relativeMask)==0027);
    int independentlyOpened=openDirectory();assert(read(independentlyOpened,bytes,32)>0);close(independentlyOpened);
    // Remove a newly returned FD before using it; a failed/interrupted caller
    // must still allow automatic provider release without a close RPC.
    int abandoned=openDirectory();close(abandoned);
    int control[2];assert(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,control));pid_t child=fork();assert(child>=0);
    if(!child) {
        close(control[0]);close(fd);close(duplicate);
        char tag=0,ancillary[CMSG_SPACE(sizeof(int))]{};iovec part{&tag,1};msghdr message{};message.msg_iov=&part;message.msg_iovlen=1;message.msg_control=ancillary;message.msg_controllen=sizeof(ancillary);
        if(recvmsg(control[1],&message,MSG_CMSG_CLOEXEC)!=1 || tag!='D')_exit(2);
        auto rights=CMSG_FIRSTHDR(&message);if(!rights || rights->cmsg_type!=SCM_RIGHTS)_exit(3);int held=-1;memcpy(&held,CMSG_DATA(rights),sizeof(held));
        if(write(control[1],"H",1)!=1 || ::read(control[1],&tag,1)!=1 || tag!='R')_exit(4);
        close(held);close(control[1]);_exit(0);
    }
    close(control[1]);char tag='D',ancillary[CMSG_SPACE(sizeof(int))]{};iovec part{&tag,1};msghdr message{};message.msg_iov=&part;message.msg_iovlen=1;message.msg_control=ancillary;message.msg_controllen=sizeof(ancillary);
    auto rights=CMSG_FIRSTHDR(&message);rights->cmsg_level=SOL_SOCKET;rights->cmsg_type=SCM_RIGHTS;rights->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(rights),&fd,sizeof(fd));
    assert(sendmsg(control[0],&message,MSG_NOSIGNAL)==1 && ::read(control[0],&tag,1)==1 && tag=='H');close(fd);close(duplicate);
    usleep(100000);assert(files.count()>=2); // Main regular file + child's directory capability.
    assert(write(control[0],"R",1)==1);close(control[0]);int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    for(unsigned attempt=0;files.count()!=1;attempt++){assert(attempt<300);usleep(10000);}
#ifdef __ANDROID__
    peer.allowFixtureProcesses=true;directoryLibcBridge(base);
    for(unsigned attempt=0;files.count()!=1;attempt++){assert(attempt<300);usleep(10000);}
    peer.allowFixtureProcesses=false;
#else
    (void)base;(void)peer;
#endif
    assert(!files.remove("/0/a/second",false,actor));
    puts("DIRECTORY_RPC_PASS unlinked_directory_cwd=1 unlinked_path_directory_cwd=1 deleted_caller_denied=1 authenticated_fchdir=1 cwd_fork_last_close=1 cwd_after_rename=1 path_chdir=1 search_identity_denied=1 search_execute_denied=1 malformed_search_denied=1 provider_metadata=1 proc_fd_descendants=1 proc_thread_self=1 proc_current_tid=1 proc_relative_mutations=1 proc_held_after_rename=1 proc_nonlink_readlink_errors=1 foreign_proc_not_aliased=1 directory_statfs=1 path_statfs=1 forged_statfs_fd_denied=1 statfs_caller_denied=1 bad_statfs_buffer=1 logical_flags=1 getdents64=1 dup_cursor=1 independent_open=1 concurrent_reads=1 seek_rewind=1 copy_failure_rollback=1 directory_sync=1 descriptor_io_errors=1 abandoned_fd_release=1 fork_scm_rights_last_close=1 relative_fd_proofs=1 relative_malformed_denied=1");
}
#endif
int main(int argc,char** argv) {
#ifdef POCKET_DIRECTORY_RPC32
    if(argc==2 && !strcmp(argv[1],"--libc-directory-client"))return directoryLibcClient();
#else
    (void)argv;
#endif
    assert(argc==1);
    auto initial=descriptorCount();
#ifdef __ANDROID__
    char temp[]="/tmp/nXXXXXX"; // Keep filesystem UNIX addresses below108 bytes after guest-root mapping.
#else
    char temp[]="/tmp/pocket-external-namespace-XXXXXX";
#endif
    assert(mkdtemp(temp));std::string base=temp;
    auto tree=base+"/system-data/media",records=base+"/identity";assert(!mkdir((base+"/system-data").c_str(),0700) && !mkdir(tree.c_str(),0700) && !mkdir(records.c_str(),0700));
    assert(!mkdir((tree+"/0").c_str(),0700) && !mkdir((base+"/dev").c_str(),0700) && !mkdir((base+"/dev/socket").c_str(),0700));
    char status[4096];auto size=readIdentityFile("/proc/self/stat",status,sizeof(status));assert(size>0);auto start=pocket_process_start(status,size);assert(start);
    PocketBinderIdentity identity{POCKET_IDENTITY_MAGIC,static_cast<uint32_t>(getpid()),10001,10002,start};auto record=records+"/"+std::to_string(getpid());
    auto saveIdentity=[&]{int fd=open(record.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);assert(fd>=0 && write(fd,&identity,sizeof(identity))==sizeof(identity));close(fd);};saveIdentity();
    std::string devicePath=base+"/.pocket-external-device",proxyPath=base+"/proxy",unboundPath=base+"/unbound";
    int proxy=open(proxyPath.c_str(),O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600),unbound=open(unboundPath.c_str(),O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);assert(proxy>=0 && unbound>=0);
    struct stat proxyStat{};assert(!fstat(proxy,&proxyStat));uint64_t device=proxyStat.st_dev;
    int fd=open(devicePath.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);assert(fd>=0 && write(fd,&device,sizeof(device))==sizeof(device));close(fd);
    {
        Peer peer;peer.owner.pid=getpid();peer.nodes.emplace(1,Peer::Node{open(tree.c_str(),O_PATH|O_DIRECTORY|O_CLOEXEC)});assert(peer.nodes.at(1).fd>=0);
        int pair[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair));Channel channel(pair[0],3000);std::thread server([&]{peer.run(pair[1]);});
        {
            Files files(channel,tree);auto actor=peer.owner;assert(!files.initialize(actor));assert(!files.mkdir("/0/a",0777,0027,actor));
            Files::Opened cap{};assert(!files.open("/0/a/file",O_RDWR|O_CREAT|O_EXCL,actor,&cap,0666,0027));
            assert(!files.bindProxy(cap.slot,cap.cookie,proxyStat.st_dev,proxyStat.st_ino));
            assert(files.bindProxy(cap.slot,cap.cookie,proxyStat.st_dev,proxyStat.st_ino)==-EALREADY);
            uint32_t written=0;assert(!files.write(cap.slot,cap.cookie,0,{'d','a','t','a'},actor,&written) && written==4);
            uid_t hostUid=0;assert(!kernelUid(&hostUid));AppFiles broker(files,getpid(),hostUid,start,records);auto socket=base+"/dev/socket/pocket_efs",locator=base+"/.pocket-external-ready";
            assert(!broker.start(socket,base+"/host",locator,devicePath));
            const std::string path="/storage/emulated/0/a/file";
            auto a=request(socket,POCKET_EXT_STAT,path);assert(!a.error && a.attributes.device==device && a.attributes.size==4 && S_ISREG(a.attributes.mode));
            auto b=request(socket,POCKET_EXT_FSTAT,"","",0,0,proxy);assert(!b.error && !memcmp(&a.attributes,&b.attributes,sizeof(a.attributes)));
            // Exercise the real C syscall-dispatch/client against this live
            // broker, not a peer returning canned namespace replies.
            for(const char* marker:{"/.pocket-external-enabled","/.pocket-external-namespace-enabled"}) {
                int enabled=open((base+marker).c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);assert(enabled>=0);close(enabled);
            }
            assert(external_namespace_fixture_init(base.c_str(),actor.uid)==1);
            auto oldMask=umask(0027);assert(external_namespace_fixture_umask()==0027);
            struct stat routed{};uint64_t args[6]={static_cast<uint64_t>(AT_FDCWD),0,reinterpret_cast<uintptr_t>(&routed),0,0,0};
            auto mapped=tree+"/0/a/file";
            assert(!external_namespace_fixture_path(SYS_newfstatat,args,path.c_str(),mapped.c_str(),""));
            assert(routed.st_ino==a.attributes.inode && routed.st_dev==a.attributes.device && routed.st_size==4);
            struct stat held{};assert(!external_namespace_fixture_fd(SYS_fstat,proxy,reinterpret_cast<uintptr_t>(&held)));
            assert(held.st_dev==routed.st_dev && held.st_ino==routed.st_ino && held.st_size==routed.st_size);
            std::string exactProxy="/proc/self/fd/"+std::to_string(proxy);struct stat followedProxy{};
            uint64_t procArgs[6]={static_cast<uint64_t>(AT_FDCWD),0,reinterpret_cast<uintptr_t>(&followedProxy),AT_EMPTY_PATH|AT_NO_AUTOMOUNT,0,0};
            assert(!external_namespace_fixture_path(SYS_newfstatat,procArgs,exactProxy.c_str(),exactProxy.c_str(),"") && S_ISREG(followedProxy.st_mode) && followedProxy.st_ino==held.st_ino && followedProxy.st_dev==held.st_dev);
            assert(external_namespace_fixture_path(SYS_chdir,procArgs,exactProxy.c_str(),exactProxy.c_str(),"")==-ENOTDIR);
            assert(external_namespace_fixture_fd(SYS_fstat,proxy,1)==-EFAULT);
            assert(external_namespace_fixture_fd(SYS_fstat,unbound,reinterpret_cast<uintptr_t>(&held))==-EBADF);
            uint64_t empty[6]={static_cast<uint64_t>(proxy),0,reinterpret_cast<uintptr_t>(&held),AT_EMPTY_PATH,0,0};
            assert(!external_namespace_fixture_path(SYS_newfstatat,empty,"","",""));
            external_namespace_fixture_mode(3);
            assert(external_namespace_fixture_path(SYS_newfstatat,args,path.c_str(),mapped.c_str(),"")==-ENOMSG);
            assert(!external_namespace_fixture_path(SYS_newfstatat,args,"/mnt/user/0/emulated/0/a/file",mapped.c_str(),""));
            assert(!external_namespace_fixture_fd(SYS_fstat,proxy,reinterpret_cast<uintptr_t>(&held)));external_namespace_fixture_mode(0);
            int lowerDir=open((tree+"/0").c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(lowerDir>=0);
            uint64_t make[6]={static_cast<uint64_t>(lowerDir),0,0777,0,0,0};
            assert(!external_namespace_fixture_path(SYS_mkdirat,make,"relative","relative",""));close(lowerDir);
            assert(!request(socket,POCKET_EXT_REMOVE,"/storage/emulated/0/relative","",AT_REMOVEDIR).error);
            uint64_t move[6]={static_cast<uint64_t>(AT_FDCWD),0,static_cast<uint64_t>(AT_FDCWD),reinterpret_cast<uintptr_t>("/tmp/escape"),0,0};
            auto outside=base+"/tmp/escape";
            assert(external_namespace_fixture_path(SYS_renameat,move,path.c_str(),mapped.c_str(),outside.c_str())==-EXDEV);
            assert(external_namespace_fixture_path(SYS_fchmodat,args,path.c_str(),mapped.c_str(),"")==-EOPNOTSUPP);
            // A final link in an internal directory must route following
            // operations to the real broker; metadata/unlink on the link
            // itself must remain a local link operation.
            auto internalLink=base+"/internal-link";
            assert(!symlink(mapped.c_str(),internalLink.c_str()));
            assert(!external_namespace_fixture_path(SYS_newfstatat,args,internalLink.c_str(),internalLink.c_str(),""));
            assert(routed.st_ino==a.attributes.inode && routed.st_size==4);
            args[3]=AT_SYMLINK_NOFOLLOW;
            assert(external_namespace_fixture_path(SYS_newfstatat,args,internalLink.c_str(),internalLink.c_str(),"")==-ENOMSG);
            args[3]=0;
            assert(external_namespace_fixture_path(SYS_fchmodat,args,internalLink.c_str(),internalLink.c_str(),"")==-EOPNOTSUPP);
            assert(external_namespace_fixture_path(SYS_fchownat,args,internalLink.c_str(),internalLink.c_str(),"")==-EOPNOTSUPP);
            args[4]=AT_SYMLINK_NOFOLLOW;
            assert(external_namespace_fixture_path(SYS_fchownat,args,internalLink.c_str(),internalLink.c_str(),"")==-ENOMSG);
            args[4]=0;
            assert(external_namespace_fixture_path(SYS_unlinkat,args,internalLink.c_str(),internalLink.c_str(),"")==-ENOMSG);
            assert(!unlink(internalLink.c_str()));
            auto dangling=base+"/dangling-link",missing=tree+"/0/a/missing";
            assert(!symlink(missing.c_str(),dangling.c_str()));
            assert(external_namespace_fixture_path(SYS_newfstatat,args,dangling.c_str(),dangling.c_str(),"")==-ENOENT);
            assert(!unlink(dangling.c_str()));
            assert(!external_namespace_fixture_fd(SYS_ftruncate,proxy,6));
            assert(!external_namespace_fixture_fd(SYS_fstat,proxy,reinterpret_cast<uintptr_t>(&held)) && held.st_size==6);
            assert(!external_namespace_fixture_fd(SYS_ftruncate,proxy,4));
            assert(umask(oldMask)==0027); // Querying the mask never changed it.
#ifdef POCKET_DIRECTORY_RPC32
            directoryProof(files,"/storage/emulated/0/a",tree+"/0/a",actor,a.attributes.inode,base,peer);
            reopenTypedProof(files,actor,tree);
#endif
            assert(request(socket,POCKET_EXT_FSTAT).error==-EINVAL);
            assert(request(socket,POCKET_EXT_STAT,path,"",0,0,proxy).error==-EINVAL);
            assert(request(socket,POCKET_EXT_FSTAT,"","",0,0,unbound).error==-EBADF);
            assert(request(socket,POCKET_EXT_FSTAT,"","",0,0,proxy,2).error==-EINVAL);
            assert(request(socket,POCKET_EXT_FSTAT,"","",0,0,proxy,9).error==-EPROTO);
            assert(request(socket,POCKET_EXT_FSTAT,"","",0,0,proxy,1,0,0,getppid()).error==-EACCES);
            identity.start_ticks++;saveIdentity();assert(request(socket,POCKET_EXT_STAT,path).error==-EACCES);identity.start_ticks--;saveIdentity();
            identity.uid=33333;saveIdentity();
            // A held, proved descriptor retains fstat metadata authority after
            // a UID change; a new pathname lookup still uses the current UID.
            auto retained=request(socket,POCKET_EXT_FSTAT,"","",0,0,proxy);assert(!retained.error && retained.attributes.inode==a.attributes.inode);
            auto denied=request(socket,POCKET_EXT_STAT,path);assert(denied.error==-EACCES && !denied.attributes.inode);
            identity.uid=actor.uid;saveIdentity();
            assert(!request(socket,POCKET_EXT_ACCESS,path,"",R_OK).error);assert(request(socket,POCKET_EXT_ACCESS,path,"",X_OK).error==-EACCES);
            assert(!request(socket,POCKET_EXT_MKDIR,"/storage/emulated/0/b","",0,0,-1,1,0777,0027).error);
            assert(!request(socket,POCKET_EXT_MKDIR,"/storage/emulated/0/a/../escape","",0,0,-1,1,0777,0027).error);
            assert(!request(socket,POCKET_EXT_REMOVE,"/storage/emulated/0/escape","",AT_REMOVEDIR).error);
            assert(request(socket,POCKET_EXT_STAT,path,"",0,0,-1,1,0600,0).error==-EINVAL);
            assert(!request(socket,POCKET_EXT_FTRUNCATE,"","",0,19,proxy).error);
            auto resized=request(socket,POCKET_EXT_STAT,path);assert(!resized.error && resized.attributes.size==19);
            int ro=open(proxyPath.c_str(),O_RDONLY|O_CLOEXEC);assert(ro>=0);assert(request(socket,POCKET_EXT_FTRUNCATE,"","",0,1,ro).error==-EINVAL);close(ro);
            assert(request(socket,POCKET_EXT_FTRUNCATE,"","",0,UINT64_MAX,proxy).error==-EINVAL);
            assert(!request(socket,POCKET_EXT_TRUNCATE,path,"",0,7).error);
            assert(!request(socket,POCKET_EXT_RENAME,path,"/mnt/user/0/emulated/0/b/moved").error);
            assert(request(socket,POCKET_EXT_STAT,path).error==-ENOENT);
            auto renamed=request(socket,POCKET_EXT_STAT,"/storage/emulated/0/b/moved");assert(!renamed.error && renamed.attributes.inode==a.attributes.inode && renamed.attributes.size==7);
            assert(request(socket,POCKET_EXT_REMOVE,"/storage/emulated/0/b","",AT_REMOVEDIR).error==-ENOTEMPTY);
            assert(!request(socket,POCKET_EXT_REMOVE,"/storage/emulated/0/b/moved").error);
            assert(request(socket,POCKET_EXT_STAT,"/storage/emulated/0/b/moved").error==-ENOENT);
            std::vector<char> bytes;assert(!files.read(cap.slot,cap.cookie,0,4,actor,&bytes) && std::string(bytes.begin(),bytes.end())=="data");
            assert(!files.release(cap.slot,cap.cookie,actor));assert(request(socket,POCKET_EXT_FSTAT,"","",0,0,proxy).error==-EBADF);
            assert(!request(socket,POCKET_EXT_REMOVE,"/storage/emulated/0/a","",AT_REMOVEDIR).error && !request(socket,POCKET_EXT_REMOVE,"/storage/emulated/0/b","",AT_REMOVEDIR).error);
            // A stale/absent guest identity cannot be replaced by a guessed
            // logical UID in a message. This new process has no identity record.
            pid_t child=fork();assert(child>=0);if(!child){auto denied=request(socket,POCKET_EXT_STAT,"/storage/emulated/0/a");_exit(denied.error==-EACCES?0:1);}
            int result;assert(waitpid(child,&result,0)==child && WIFEXITED(result) && WEXITSTATUS(result)==0);
            broker.stop();assert(files.count()==0 && broker.namespaceQueries>=12);
            assert(!files.remove("/0",true,actor));
        }
        channel.cancel();server.join();assert(peer.handles.empty());for(auto n:peer.nodes)assert(!n.second.references);
    }
    close(proxy);close(unbound);assert(!unlink(record.c_str()) && !unlink(devicePath.c_str()) && !unlink(proxyPath.c_str()) && !unlink(unboundPath.c_str()));
    for(const char* marker:{"/.pocket-external-enabled","/.pocket-external-namespace-enabled"})assert(!unlink((base+marker).c_str()));
    assert(!rmdir(tree.c_str()) && !rmdir((base+"/system-data").c_str()) && !rmdir(records.c_str()) && !rmdir((base+"/dev/socket").c_str()) && !rmdir((base+"/dev").c_str()) && !rmdir(base.c_str()) && descriptorCount()==initial);
    puts("EXTERNAL_NAMESPACE_RPC_PASS live_pid_start_tid=1 descriptor_proof=1 guessed_cap_denied=1 stale_cap_denied=1 partial_header_rights=1 excess_rights_closed=1 path_fd_attrs_agree=1 real_filesystem_mutations=1 readonly_truncate_denied=1 cross_process_denied=1 loader_dispatch_client=1 missing_relative_mkdir=1 cross_mount_rename_denied=1 bad_user_buffer=1 mask_not_changed=1 forced_provider_fd=1 lookup_refs_and_handles_balanced=1 fd_leaks=0");
}
