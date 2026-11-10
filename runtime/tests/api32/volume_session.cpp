// SPDX-License-Identifier: GPL-3.0-only
#include "fuse_namespace_peer.h"
#include "../../api32/external_volume.h"
#include "../../api32/external_app_dirs.h"
#include "volume_adapter_errors.h"
#include "received_rights_peer.h"
#include <sys/mman.h>
using namespace pocket::fuse32;
static std::string path(const char* run,const char* phase){return std::string("/tmp/volume-")+run+"-"+phase;}
static void publish(const std::string& name){int fd=open(name.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);assert(fd>=0 && write(fd,"0\n",2)==2 && !close(fd));}
static void await(const std::string& name){for(unsigned n=0;;n++){char bytes[8]{};ssize_t count=readIdentityFile(name.c_str(),bytes,sizeof(bytes));if(count==2 && !memcmp(bytes,"0\n",2))return;assert(n<2000);usleep(20000);}}
static int loseDetachReply(const ExternalVolume::Options& options,uint64_t epoch) {
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);assert(fd>=0);
    sockaddr_un address{};address.sun_family=AF_UNIX;auto endpoint=options.directory+"/pocket_volume32";assert(endpoint.size()<sizeof(address.sun_path));memcpy(address.sun_path,endpoint.c_str(),endpoint.size()+1);
    assert(!connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)) && !hostPeer(fd,options.hostPid,options.hostUid));
    // Disable replies before publishing the request. The authenticated host
    // must finish revocation, observe EPIPE, and accept the same-epoch retry.
    assert(!shutdown(fd,SHUT_RD));VolumeRequest request{VolumeMagic,1,2,0,epoch};
    assert(send(fd,&request,sizeof(request),MSG_NOSIGNAL)==sizeof(request) && !close(fd));return -ECONNRESET;
}
static void appDirectories() {
    using pocket::storage32::AppDirectories;AppDirectories::Plan plan;
    assert(!AppDirectories::parse("/storage/emulated/0/Android/data/dev.kreijstal.directorytest/files/",10001,&plan));
    int root=open("/data/media/0",O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(root>=0);
    assert(!AppDirectories::prepare(root,plan,AppDirectories::Operation::FixExisting));
    assert(access("/data/media/0/Android",F_OK)==-1 && errno==ENOENT);
    assert(!AppDirectories::prepare(root,plan,AppDirectories::Operation::Setup));
    const char* files="/data/media/0/Android/data/dev.kreijstal.directorytest/files";struct stat st{};
    assert(!stat(files,&st) && st.st_uid==10001 && st.st_gid==1078 && (st.st_mode&07777)==02770);
    assert(!chmod(files,0700) && !AppDirectories::prepare(root,plan,AppDirectories::Operation::EnsureExists));assert(!stat(files,&st) && (st.st_mode&07777)==0700);
    std::string canary=std::string(files)+"/canary";int fd=open(canary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);assert(fd>=0 && write(fd,"data",4)==4 && !close(fd));
    assert(!AppDirectories::prepare(root,plan,AppDirectories::Operation::FixExisting));assert(!stat(canary.c_str(),&st) && st.st_uid==10001 && st.st_gid==1078);
    assert(!unlink(canary.c_str()) && !rmdir(files));
    for(const char* area:{"obb","media"}) {
        assert(!AppDirectories::parse(std::string("/storage/emulated/0/Android/")+area+"/dev.kreijstal.directorytest",10001,&plan));
        assert(!AppDirectories::prepare(root,plan,AppDirectories::Operation::Setup));
        std::string path=std::string("/data/media/0/Android/")+area+"/dev.kreijstal.directorytest";
        assert(!stat(path.c_str(),&st) && st.st_uid==10001 && st.st_gid==(!strcmp(area,"obb")?1079:1023));assert(!rmdir(path.c_str()));
    }
    assert(!rmdir("/data/media/0/Android/data/dev.kreijstal.directorytest"));
    for(const char* area:{"data","obb","media"})assert(!rmdir((std::string("/data/media/0/Android/")+area).c_str()));
    assert(!rmdir("/data/media/0/Android") && !close(root));
    puts("VOLUME_APP_DIRECTORIES32_PASS setup=1 ensure_existing=1 fix_missing=1 fixup_entries=1 actual_guest_uid_gid_metadata=1 user0_only=1 host_kernel_owner_changed=0");
}
static void lowerCanonical() {
    assert(syscall(__NR_getpid,0x504c2001UL,6UL,3UL)==0);
    for(const char* input:{"/storage/emulated/0/a/file","/sdcard/a/file"}) {
        char* canonical=realpath(input,nullptr);assert(canonical && !strcmp(canonical,"/storage/emulated/0/a/file"));free(canonical);
    }
    assert(syscall(__NR_getpid,0x504c2001UL,6UL,0UL)==0);
    puts("VOLUME_LOWER_CANONICAL32_PASS pass_through=1 bionic_realpath=1 storage_alias=1 sdcard_alias=1 view_restored=1");
}
static void pathHandles() {
    char* canonical=realpath("/sdcard/a//./",nullptr);assert(canonical && !strcmp(canonical,"/storage/emulated/0/a"));free(canonical);
    canonical=realpath("/storage/self/primary/a/file",nullptr);assert(canonical && !strcmp(canonical,"/storage/emulated/0/a/file"));free(canonical);
    int fd=open("/sdcard/a/file",O_PATH|O_RDWR|O_TRUNC|O_CLOEXEC);assert(fd>=0 && fcntl(fd,F_GETFL)==O_PATH && (fcntl(fd,F_GETFD)&FD_CLOEXEC));
    struct stat st{},named{};assert(!fstat(fd,&st) && !stat("/sdcard/a/file",&named) && st.st_dev==named.st_dev && st.st_ino==named.st_ino && st.st_size==8192);
    char bytes[256]{};errno=0;assert(read(fd,bytes,1)==-1 && errno==EBADF);errno=0;assert(fsync(fd)==-1 && errno==EBADF);
    auto link=[&](int cap){char name[64];snprintf(name,sizeof(name),"/proc/self/fd/%d",cap);ssize_t n=readlink(name,bytes,sizeof(bytes));assert(n>0);return std::string(bytes,n);};
    assert(link(fd)=="/storage/emulated/0/a/file");
    int duplicate=fcntl(fd,F_DUPFD_CLOEXEC,0);assert(duplicate>=0 && !close(fd));fd=duplicate;
    assert(!rename("/sdcard/a/file","/sdcard/a/moved") && link(fd)=="/storage/emulated/0/a/moved");
    assert(!rename("/sdcard/a/moved","/sdcard/a/file") && !close(fd));
    int directory=open("/sdcard/a",O_PATH|O_DIRECTORY|O_CLOEXEC);assert(directory>=0);
    errno=0;assert(unlink("/sdcard/a")==-1 && errno==EISDIR);errno=0;assert(rmdir("/sdcard/a/file")==-1 && errno==ENOTDIR);
    assert(!mkdir("/sdcard/a/type-dir",0777));errno=0;assert(unlink("/sdcard/a/type-dir")==-1 && errno==EISDIR);assert(!rmdir("/sdcard/a/type-dir"));
    puts("VOLUME_UNLINK_TYPES32_PASS unlink_dir_eisdir=1 rmdir_file_enotdir=1 empty_dir_rmdir=1 provider_assumes_kernel_type_checks=1");
    fd=openat(directory,"file",O_PATH|O_CLOEXEC);assert(fd>=0 && link(fd)=="/storage/emulated/0/a/file" && !close(fd));
    fd=openat(directory,"path-unlink",O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0666);assert(fd>=0 && link(fd)=="/storage/emulated/0/a/path-unlink" && !close(fd));
    fd=openat(directory,"path-unlink",O_PATH|O_CLOEXEC);assert(fd>=0 && !unlinkat(directory,"path-unlink",0));
    assert(link(fd)=="/storage/emulated/0/a/path-unlink (deleted)" && !fstat(fd,&st) && !st.st_nlink && !close(fd) && !close(directory));
    puts("VOLUME_PATH32_PASS bionic_realpath=1 sdcard_alias=1 path_only_flags=1 metadata=1 proc_link=1 rename_unlink=1 relative_path_fd=1 regular_proxy_link=1");
}
static void heldAnchor(const char* run) {
    int meta=open("/sdcard/a/file",O_PATH|O_CLOEXEC),data=open("/sdcard/a/file",O_RDONLY|O_CLOEXEC);assert(meta>=0 && data>=0);
    struct stat original{},held{};assert(!fstat(meta,&original));publish(path(run,"external-held"));await(path(run,"external-moved"));
    // Prove the injected provider path failure is live. A new path handle
    // must not bypass visibility policy, while already granted FDs retain
    // the renamed inode and its working provider data handle.
    errno=0;assert(stat("/sdcard/a/external-moved",&held)==-1 && errno==ENOENT);
    errno=0;assert(open("/sdcard/a/external-moved",O_PATH|O_CLOEXEC)==-1 && errno==ENOENT);
    assert(!fstat(meta,&held) && held.st_ino==original.st_ino && held.st_size==8192 && !fstat(data,&held) && held.st_ino==original.st_ino);
    char bytes[64]{};assert(pread(data,bytes,17,0)==17 && !memcmp(bytes,"pocket-volume32--",17));
    char link[64];snprintf(link,sizeof(link),"/proc/self/fd/%d",meta);ssize_t n=readlink(link,bytes,sizeof(bytes));assert(n>0 && std::string(bytes,n)=="/storage/emulated/0/a/external-moved");
    publish(path(run,"external-checked"));await(path(run,"external-restored"));assert(!close(meta) && !close(data));
    puts("VOLUME_HELD_ANCHOR32_PASS provider_getattr_enoent=1 new_path_open_denied=1 opath_fstat=1 data_fstat=1 data_read=1 external_rename=1");
}
static int client(const char* run) {
    assert(!setgid(10002) && !setuid(10001));umask(0027);assert(getuid()==10001 && getgid()==10002);
    errno=0;assert(syscall(__NR_getpid,0x504c2001UL,8UL,1UL)==-1 && errno==EPERM);
    int control=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);assert(control>=0);sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/pocket_volume32");assert(!connect(control,reinterpret_cast<sockaddr*>(&address),sizeof(address)));char tag;assert(read(control,&tag,1)==0 && !close(control));
    pathHandles();heldAnchor(run);
    DIR* stream=opendir("/storage/emulated/0/a");assert(stream);int directory=dirfd(stream);struct stat named{},held{};
    int fd=openat(directory,"file",O_RDWR|O_CLOEXEC);assert(fd>=0 && !fstat(fd,&held) && !fstatat(directory,"file",&named,0));assert(held.st_dev==named.st_dev && held.st_ino==named.st_ino && held.st_size==8192);
    char bytes[64]{};assert(pread(fd,bytes,17,0)==17 && !memcmp(bytes,"pocket-volume32--",17));
    assert(pwrite(fd,"proxy-write",11,128)==11 && !fsync(fd) && !fdatasync(fd));
    void* memory=mmap(nullptr,8192,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);assert(memory!=MAP_FAILED);memcpy(static_cast<char*>(memory)+256,"shared-volume-map",17);assert(!msync(memory,8192,MS_SYNC) && !munmap(memory,8192));
    assert(!ftruncate(fd,8192) && !close(fd));
    fd=openat(directory,"created",O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0666);assert(fd>=0 && pwrite(fd,"new",3,0)==3 && !close(fd));assert(!unlinkat(directory,"created",0));
    int heldFile=openat(directory,"file",O_RDONLY|O_CLOEXEC);assert(heldFile>=0);
    assert(pread(heldFile,bytes,17,0)==17 && !memcmp(bytes,"pocket-volume32--",17));
    void* heldMapping=mmap(nullptr,8192,PROT_READ,MAP_SHARED,heldFile,0);assert(heldMapping!=MAP_FAILED);
    assert(!memcmp(heldMapping,"pocket-volume32--",17));
    printf("VOLUME_CLIENT32_READY pid=%d uid=10001 ordinary_openat=1 real_appfuse_data=1 path_fd_attrs=1 create_unlink=1 shared_mmap=1 scoped_kernel_denied=1 host_adapter_peer_denied=1 cached_read=1 held_mapping=1\n",getpid());
    publish(path(run,"client-ready"));await(path(run,"revoked"));
    // AOSP terminates volume holders before unmount. A disconnected FUSE
    // channel alone cannot revoke cached pages. Reaching this point is a
    // failure; the outer fixture independently requires SIGKILL and this PID
    // in the authenticated adapter's retirement report.
    fputs("VOLUME_CLIENT32_NOT_RETIRED\n",stderr);return 1;
}
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);const char* run=getenv("POCKET_TEST_RUN");assert(run && strlen(run)==32);
    if(argc==2 && !strcmp(argv[1],"--app-client"))return client(run);
    assert(argc==1 && getuid()==0);const char* pid=getenv("POCKET_API32_HOST_PID"),*start=getenv("POCKET_API32_HOST_START");assert(pid && start);
    volumeAdapterErrors("/tmp");
    receivedRightsErrors32();
    {ExternalKernelScope lower;assert(lower.okay());assert(!mkdir("/storage",0755) && !mkdir("/storage/self",0755));assert(!symlink("/data/media","/storage/emulated") && !symlink("../emulated/0","/storage/self/primary") && !symlink("/storage/emulated/0","/sdcard"));}
    errno=0;assert(symlink("/data/media","/storage/emulated")==-1 && errno==EOPNOTSUPP);
    ExternalVolume::Options options;options.hostPid=atoi(pid);options.hostStart=strtoull(start,nullptr,10);assert(!kernelUid(&options.hostUid));
    assert(!mkdir("/data/media/0/a",0750));int fd=open("/data/media/0/a/file",O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0640);assert(fd>=0);std::vector<char> data(8192,'Q');memcpy(data.data(),"pocket-volume32--",17);assert(write(fd,data.data(),data.size())==static_cast<ssize_t>(data.size()) && !close(fd));
    appDirectories();lowerCanonical();
    Peer peer;peer.owner.pid=getpid();peer.allowFixtureProcesses=true;peer.allowKernelNoCaller=true;peer.requireVfsTypeChecks=true;peer.hostAdapter=options.hostPid;peer.nodes.emplace(1,Peer::Node{open("/data/media",O_PATH|O_DIRECTORY|O_CLOEXEC)});assert(peer.nodes.at(1).fd>=0);std::thread daemon;
    const bool retry=getenv("POCKET_TEST_DETACH_RETRY") && !strcmp(getenv("POCKET_TEST_DETACH_RETRY"),"1");bool lost=false;
    ExternalVolume volume(options,[&](unsigned op,uint64_t epoch){if(retry && op==2 && !lost){lost=true;return loseDetachReply(options,epoch);}return volumeAdapter(options,op,epoch);});
    int result=volume.mount([&](int endpoint){int copy=dup(endpoint);assert(copy>=0);daemon=std::thread([&,copy]{peer.run(copy);});return 0;},peer.owner);
    printf("VOLUME_MOUNT_RESULT=%d\n",result);assert(!result && volume.mounted());publish(path(run,"ready"));
    await(path(run,"external-held"));struct stat original{};assert(!stat("/data/media/0/a/file",&original));
    assert(!rename("/data/media/0/a/file","/data/media/0/a/external-moved"));peer.staleGetattrInode=original.st_ino;publish(path(run,"external-moved"));
    await(path(run,"external-checked"));assert(!rename("/data/media/0/a/external-moved","/data/media/0/a/file"));peer.staleGetattrInode=0;publish(path(run,"external-restored"));
    await(path(run,"revoke"));
    if(retry) {
        assert(volume.stop()==-ECONNRESET && !volume.clean() && !volume.mounted());
        assert(!access((options.directory+"/pocket_ext").c_str(),F_OK));
        assert(volume.mount([](int){assert(false);return -EIO;},peer.owner)==-EBUSY);
    }
    assert(!volume.stop() && volume.clean() && !volume.mounted());daemon.join();assert(peer.handles.empty() && peer.kernelNoCaller>0);for(const auto& node:peer.nodes)assert(!node.second.references);
    if(retry)puts("VOLUME_DETACH_RETRY32_PASS lost_reply=1 callback_broker_retained=1 duplicate_mount_denied=1 same_epoch_retry=1 provider_handles_balanced=1");
    publish(path(run,"revoked"));
    fd=open("/data/media/0/a/file",O_RDONLY|O_CLOEXEC);assert(fd>=0);assert(pread(fd,data.data(),data.size(),0)==static_cast<ssize_t>(data.size()));assert(!memcmp(data.data()+128,"proxy-write",11) && !memcmp(data.data()+256,"shared-volume-map",17));close(fd);
    assert(!unlink("/data/media/0/a/file") && !rmdir("/data/media/0/a"));
    puts("VOLUME_SESSION32_PASS init_callback=1 authenticated_host_adapter=1 actual_appfuse_device=1 ordinary_guest_uid=10001 scoped_proxy_metadata=1 revoke_with_cached_holder=1 backing_data=1 provider_handles_and_refs_balanced=1 no_caller_context_preserved=1 bootstrap_lower_scope=1 unscoped_namespace_preserved=1 native_core_embedding_model=1 real_mediaprovider_claim=0");return 0;
}
