// SPDX-License-Identifier: GPL-3.0-only
#include "fuse_namespace_peer.h"
#include "../../api32/external_open_rpc.h"
extern "C" int external_namespace_fixture_init(const char*,unsigned);
extern "C" long external_namespace_fixture_fd(long,int,uintptr_t);
extern "C" long external_namespace_fixture_directory(long,int,uintptr_t,uint64_t);
extern "C" long external_namespace_fixture_path(long,const uint64_t[6],const char*,const char*,const char*);
static unsigned descriptorCount(){DIR* d=opendir("/proc/self/fd");assert(d);unsigned n=0;while(readdir(d))n++;closedir(d);return n;}
int main() {
    unsigned initial=descriptorCount();char temp[]="/tmp/pocket-path-XXXXXX";assert(mkdtemp(temp));std::string root=temp,tree=root+"/system-data/media";
    for(auto path:{root+"/system-data",tree,tree+"/0",tree+"/0/a",root+"/identity",root+"/dev",root+"/dev/socket"})assert(!mkdir(path.c_str(),0700));
    auto save=[](const std::string& name,const void* data,size_t size){int fd=open(name.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);assert(fd>=0);if(size)assert(write(fd,data,size)==static_cast<ssize_t>(size));assert(!close(fd));};
    save(tree+"/0/a/file","data",4);
    for(const char* marker:{"/.pocket-external-enabled","/.pocket-external-namespace-enabled"})save(root+marker,nullptr,0);
    char status[4096];ssize_t count=readIdentityFile("/proc/self/stat",status,sizeof(status));assert(count>0);
    PocketBinderIdentity identity{POCKET_IDENTITY_MAGIC,static_cast<uint32_t>(getpid()),10001,10002,pocket_process_start(status,count)};assert(identity.start_ticks);
    auto record=root+"/identity/"+std::to_string(getpid());save(record,&identity,sizeof(identity));
    uint64_t device=73;save(root+"/.pocket-external-device",&device,sizeof(device));
    {
        Peer peer;peer.owner.pid=getpid();peer.requireVfsTypeChecks=true;peer.nodes.emplace(1,Peer::Node{open(tree.c_str(),O_PATH|O_DIRECTORY|O_CLOEXEC)});assert(peer.nodes.at(1).fd>=0);
        int pair[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair));Channel channel(pair[0],3000);std::thread daemon([&]{peer.run(pair[1]);});
        {
            Files files(channel,tree);assert(!files.initialize(peer.owner));
            uid_t hostUid=0;assert(!kernelUid(&hostUid));AppFiles broker(files,getpid(),hostUid,identity.start_ticks,root+"/identity");
            assert(!broker.start(root+"/dev/socket/pocket_efs",root+"/host",root+"/.pocket-external-ready",root+"/.pocket-external-device"));
            assert(external_namespace_fixture_init(root.c_str(),identity.uid)==1);
            auto opened=[&](const std::string& suffix,unsigned flags){uint64_t args[6]={static_cast<uint64_t>(AT_FDCWD),0,flags,07777,0,0};auto name="/storage/emulated"+suffix,lower=tree+suffix;return external_namespace_fixture_path(SYS_openat,args,name.c_str(),lower.c_str(),"");};
            auto link=[&](int fd,const std::string& prefix="/proc/self/fd/",uintptr_t output=0,size_t size=4096){char bytes[4096]{};auto name=prefix+std::to_string(fd);uint64_t args[6]={static_cast<uint64_t>(AT_FDCWD),0,output?output:reinterpret_cast<uintptr_t>(bytes),size,0,0};long n=external_namespace_fixture_path(SYS_readlinkat,args,name.c_str(),name.c_str(),"");return std::make_pair(n,n>0 && !output?std::string(bytes,n):std::string());};
            int fd=opened("/0/a/file",O_PATH|O_CLOEXEC|O_RDWR|O_CREAT|O_TRUNC|O_APPEND);assert(fd>=0);
            assert((external_namespace_fixture_directory(SYS_fcntl,fd,F_GETFL,0))==O_PATH && (fcntl(fd,F_GETFD)&FD_CLOEXEC));
            struct stat held{},named{};assert(!external_namespace_fixture_fd(SYS_fstat,fd,reinterpret_cast<uintptr_t>(&held)) && S_ISREG(held.st_mode) && held.st_size==4);
            uint64_t statArgs[6]={static_cast<uint64_t>(AT_FDCWD),0,reinterpret_cast<uintptr_t>(&named),0,0,0};
            auto name=std::string("/storage/emulated/0/a/file"),lower=tree+"/0/a/file";
            assert(!external_namespace_fixture_path(SYS_newfstatat,statArgs,name.c_str(),lower.c_str(),""));assert(held.st_dev==named.st_dev && held.st_ino==named.st_ino);
            assert(link(fd).second==name && link(fd,"/proc/thread-self/fd/").second==name && link(fd,"/proc/"+std::to_string(getpid())+"/fd/").second==name);
            assert(link(fd,"/proc/self/fd/",0,3).second=="/st" && link(fd,"/proc/self/fd/",1).first==-EFAULT && link(fd,"/proc/self/fd/",0,0).first==-EINVAL);
            for(auto nr:{SYS_read,SYS_write,SYS_fsync,SYS_fdatasync,SYS_ftruncate,SYS_getdents64})assert(external_namespace_fixture_fd(nr,fd,0)==-EBADF);
            assert(external_namespace_fixture_directory(SYS_fcntl,fd,F_SETFL,O_NONBLOCK)==-EBADF);
            // A peer can reproduce a carrier's public tag using its own real
            // SO_COOKIE. The broker must also require its private registry.
            int forged=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);assert(forged>=0);uint64_t cookie=0;socklen_t cookieSize=sizeof(cookie);
            assert(!getsockopt(forged,SOL_SOCKET,SO_COOKIE,&cookie,&cookieSize));sockaddr_un address{};address.sun_family=AF_UNIX;
            memcpy(address.sun_path+1,POCKET_EXTERNAL_PATH_TAG,POCKET_EXTERNAL_PATH_TAG_SIZE);memcpy(address.sun_path+1+POCKET_EXTERNAL_PATH_TAG_SIZE,&cookie,sizeof(cookie));
            assert(!bind(forged,reinterpret_cast<sockaddr*>(&address),offsetof(sockaddr_un,sun_path)+1+POCKET_EXTERNAL_PATH_TAG_SIZE+sizeof(cookie)));
            assert(external_namespace_fixture_fd(SYS_fstat,forged,reinterpret_cast<uintptr_t>(&named))==-EBADF && link(forged).first==-EBADF && !close(forged));
            int duplicate=fcntl(fd,F_DUPFD_CLOEXEC,0);assert(duplicate>=0 && !close(fd));fd=duplicate;
            assert(!rename(lower.c_str(),(tree+"/0/a/renamed").c_str()));assert(link(fd).second=="/storage/emulated/0/a/renamed");
            peer.staleGetattrInode=held.st_ino;
            assert(!external_namespace_fixture_fd(SYS_fstat,fd,reinterpret_cast<uintptr_t>(&named)) && named.st_ino==held.st_ino);
            assert(!rename((tree+"/0/a/renamed").c_str(),(root+"/outside").c_str()) && link(fd).first==-EXDEV);
            assert(!rename((root+"/outside").c_str(),(tree+"/0/a/renamed").c_str()));
            assert(!unlink((tree+"/0/a/renamed").c_str()));assert(link(fd).second=="/storage/emulated/0/a/renamed (deleted)");
            assert(!external_namespace_fixture_fd(SYS_fstat,fd,reinterpret_cast<uintptr_t>(&held)) && held.st_nlink==0 && !close(fd));
            peer.staleGetattrInode=0;
            assert(opened("/0/a/missing",O_PATH|O_CREAT)==-ENOENT && access((tree+"/0/a/missing").c_str(),F_OK)<0);
            save(tree+"/0/a/file","data",4);assert(opened("/0/a/file",O_PATH|O_DIRECTORY)==-ENOTDIR && opened("/0/a/file/",O_PATH)==-ENOTDIR);
            assert(!stat((tree+"/0/a/file").c_str(),&named));peer.staleGetattrInode=named.st_ino;
            assert(opened("/0/a/file",O_PATH)==-ENOENT);peer.staleGetattrInode=0;
            assert(files.remove("/0/a/file/",false,peer.owner)==-EOPNOTSUPP && files.rename("/0/a/file/","/0/a/other",peer.owner)==-EOPNOTSUPP && !access((tree+"/0/a/file").c_str(),F_OK));
            assert(files.remove("/0/a",false,peer.owner)==-EISDIR && files.remove("/0/a/file",true,peer.owner)==-ENOTDIR);
            int directory=opened("/0/a//./",O_PATH|O_DIRECTORY);assert(directory>=0);
            assert(!rename((tree+"/0/a").c_str(),(tree+"/0/moved").c_str()) && link(directory).second=="/storage/emulated/0/moved");
            uint64_t relative[6]={static_cast<uint64_t>(directory),0,O_PATH,0,0,0};
            fd=external_namespace_fixture_path(SYS_openat,relative,"file","file","");assert(fd>=0 && link(fd).second=="/storage/emulated/0/moved/file" && !close(fd));
            assert(!unlink((tree+"/0/moved/file").c_str()) && !rmdir((tree+"/0/moved").c_str()));assert(link(directory).second=="/storage/emulated/0/moved (deleted)" && !close(directory));
            for(unsigned n=0;files.count();n++){assert(n<300);usleep(10000);}broker.stop();
            assert(!peer.creates && !peer.releases); // O_PATH never acquires a data FH.
            save(tree+"/0/data","data",4);Files::Opened regular{};assert(!files.open("/0/data",O_RDONLY,peer.owner,&regular));
            fuse_attr attr{};assert(!files.attributes(regular.slot,regular.cookie,peer.owner,&attr));peer.staleGetattrInode=attr.ino;
            assert(!rename((tree+"/0/data").c_str(),(tree+"/0/moved").c_str()));uint64_t size=0;
            assert(!files.size(regular.slot,regular.cookie,&size) && size==4 && !files.attributes(regular.slot,regular.cookie,peer.owner,&attr));
            assert(!unlink((tree+"/0/moved").c_str()) && !files.attributes(regular.slot,regular.cookie,peer.owner,&attr) && !attr.nlink);
            assert(!files.release(regular.slot,regular.cookie,peer.owner));peer.staleGetattrInode=0;
        }
        channel.cancel();daemon.join();assert(peer.handles.empty());for(const auto& item:peer.nodes)assert(!item.second.references);
    }
    assert(!unlink(record.c_str()) && !unlink((root+"/.pocket-external-device").c_str()));
    for(const char* marker:{"/.pocket-external-enabled","/.pocket-external-namespace-enabled"})assert(!unlink((root+marker).c_str()));
    for(auto path:{tree+"/0",tree,root+"/system-data",root+"/identity",root+"/dev/socket",root+"/dev",root})assert(!rmdir(path.c_str()));
    assert(descriptorCount()==initial);puts("PATH_HANDLES_PASS ignored_open_flags=1 provider_lookup_without_open=1 metadata=1 canonical_proc_link=1 rename_unlink_kernel_tracking=1 held_parent_relative=1 data_ops_ebadf=1 dup_last_close=1 invalid_buffers=1 refs_balanced=1 fd_leaks=0");
}
