// SPDX-License-Identifier: GPL-3.0-only
// Stateful wire peer backed by real temporary Linux files, with a lookup-ref
// ledger. This checks namespace semantics and lifetime, not MediaProvider ACLs.
#include "../../api32/fuse_files.h"
#include <cassert>
#include <cstdio>
#include <dirent.h>
#include <set>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
using namespace pocket::fuse32;

#pragma once
struct Peer {
    struct Node {int fd;uint64_t references=0;};
    std::map<uint64_t,Node> nodes;
    std::map<uint64_t,int> handles;
    uint64_t nextNode=2,nextHandle=10;
    unsigned creates=0,forgets=0,releases=0,denials=0;
    std::atomic<bool> race{false},badDirectory{false};
    std::atomic<bool> allowFixtureProcesses{false};
    std::atomic<bool> allowKernelNoCaller{false};
    std::atomic<unsigned> kernelNoCaller{0};
    // Actual MediaProvider can retain an old pathname after a lower rename;
    // its GETATTR then returns ENOENT although a held inode is still live.
    std::atomic<uint64_t> staleGetattrInode{0};
    std::atomic<bool> requireVfsTypeChecks{false};
    pid_t hostAdapter=0;
    std::set<uint32_t> knownTids;
    Identity owner{10001,10002,20001};
    static fuse_attr attr(int fd) {
        struct stat st{};assert(fstat(fd,&st)==0);fuse_attr a{};
        a.ino=st.st_ino;a.size=st.st_size;a.blocks=st.st_blocks;
        a.mode=st.st_mode;a.nlink=st.st_nlink;a.uid=st.st_uid;a.gid=st.st_gid;a.blksize=st.st_blksize;
        a.atime=st.st_atim.tv_sec;a.mtime=st.st_mtim.tv_sec;a.ctime=st.st_ctim.tv_sec;
        a.atimensec=st.st_atim.tv_nsec;a.mtimensec=st.st_mtim.tv_nsec;a.ctimensec=st.st_ctim.tv_nsec;a.rdev=st.st_rdev;
        return a;
    }
    fuse_entry_out entry(int parent,const char* name) {
        int fd=openat(parent,name,O_PATH|O_NOFOLLOW|O_CLOEXEC);assert(fd>=0);
        auto a=attr(fd);uint64_t id=0;
        for(auto& n:nodes)if(attr(n.second.fd).ino==a.ino){id=n.first;break;}
        if(!id){id=nextNode++;nodes.emplace(id,Node{fd});}else close(fd);
        nodes.at(id).references++;fuse_entry_out e{};e.nodeid=id;e.attr=a;return e;
    }
    ~Peer(){for(auto h:handles)close(h.second);for(auto n:nodes)close(n.second.fd);}
    void run(int fd) {
        std::vector<char> packet(Channel::MaxPacket);
        for(;;) {
            auto count=recv(fd,packet.data(),packet.size(),0);if(!count)break;
            assert(count>=static_cast<ssize_t>(sizeof(RequestHeader)));
            RequestHeader h{};memcpy(&h,packet.data(),sizeof(h));assert(h.length==count);
            auto body=packet.data()+sizeof(h);size_t size=count-sizeof(h);std::vector<char> out;int error=0;
            auto respond=[&](const auto& v){out.resize(sizeof(v));memcpy(out.data(),&v,sizeof(v));};
            auto exactName=[&](size_t prefix=0) {
                assert(size>prefix && memchr(body+prefix,0,size-prefix)==body+size-1);return body+prefix;
            };
            std::string task="/proc/"+std::to_string(getpid())+"/task/"+std::to_string(h.pid);
            bool valid=h.pid==owner.pid || (owner.pid==static_cast<uint32_t>(getpid()) && access(task.c_str(),F_OK)==0);
            if(allowKernelNoCaller && !h.pid && (h.opcode==FUSE_READ || h.opcode==FUSE_WRITE || h.opcode==FUSE_FSYNC || h.opcode==FUSE_FLUSH || h.opcode==FUSE_RELEASE)){valid=true;kernelNoCaller++;}
            // A non-debuggable host parent's proc files may be hidden from
            // its child. The known adapter's real TID can still be checked
            // without reading proc or inventing a replacement caller PID.
            if(hostAdapter>0 && h.pid>0 && !syscall(__NR_tgkill,hostAdapter,h.pid,0))valid=true;
            if(allowFixtureProcesses){std::string process="/proc/"+std::to_string(h.pid)+"/stat";valid=valid || knownTids.count(h.pid) || access(process.c_str(),F_OK)==0;if(valid)knownTids.insert(h.pid);}
            if(!valid || h.gid!=owner.gid)fprintf(stderr,"FUSE_PEER_CALLER_REJECT opcode=%u uid=%u gid=%u pid=%u expected_gid=%u\n",h.opcode,h.uid,h.gid,h.pid,owner.gid);
            assert(valid && h.gid==owner.gid);
            if(h.node>FUSE_ROOT_ID)assert(nodes.at(h.node).references>0);
            if(h.uid!=owner.uid){denials++;error=-EACCES;}
            else switch(h.opcode) {
            case FUSE_INIT:{fuse_init_out v{};v.major=7;v.minor=31;v.max_write=4096;v.flags=FUSE_DO_READDIRPLUS;respond(v);break;}
            case FUSE_LOOKUP:{const char* name=exactName();struct stat st{};
                if(fstatat(nodes.at(h.node).fd,name,&st,AT_SYMLINK_NOFOLLOW)){error=-errno;break;}
                respond(entry(nodes.at(h.node).fd,name));break;}
            case FUSE_GETATTR:{assert(size==sizeof(fuse_getattr_in));fuse_getattr_in in{};memcpy(&in,body,sizeof(in));
                fuse_attr_out v{};v.attr=attr(in.getattr_flags&FUSE_GETATTR_FH?handles.at(in.fh):nodes.at(h.node).fd);
                if(staleGetattrInode && v.attr.ino==staleGetattrInode){error=-ENOENT;break;}respond(v);break;}
            case FUSE_ACCESS:{assert(size==sizeof(fuse_access_in));fuse_access_in in{};memcpy(&in,body,sizeof(in));
                std::string path="/proc/self/fd/"+std::to_string(nodes.at(h.node).fd);
                if(faccessat(AT_FDCWD,path.c_str(),in.mask,0))error=-errno;
                break;}
            case FUSE_STATFS:{assert(!size);struct statvfs fs{};
                if(fstatvfs(nodes.at(h.node).fd,&fs)){error=-errno;break;}
                fuse_statfs_out value{};value.st.blocks=fs.f_blocks;value.st.bfree=fs.f_bfree;value.st.bavail=fs.f_bavail;
                value.st.files=fs.f_files;value.st.ffree=fs.f_ffree;value.st.bsize=fs.f_bsize;value.st.frsize=fs.f_frsize;value.st.namelen=fs.f_namemax;
                respond(value);break;}
            case FUSE_MKDIR:{fuse_mkdir_in in{};assert(size>sizeof(in));memcpy(&in,body,sizeof(in));
                assert(in.mode==(S_IFDIR|0750) && in.umask==0027);
                const char* name=exactName(sizeof(in));if(mkdirat(nodes.at(h.node).fd,name,in.mode)){error=-errno;break;}
                respond(entry(nodes.at(h.node).fd,name));break;}
            case FUSE_CREATE:{fuse_create_in in{};assert(size>sizeof(in));memcpy(&in,body,sizeof(in));creates++;
                assert(in.mode==(S_IFREG|0640) && in.umask==0027);
                const char* name=exactName(sizeof(in));
                if(race){int raced=openat(nodes.at(h.node).fd,name,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);assert(raced>=0);close(raced);race=false;error=-EEXIST;break;}
                int file=openat(nodes.at(h.node).fd,name,in.flags|O_CLOEXEC,in.mode);if(file<0){error=-errno;break;}
                auto e=entry(nodes.at(h.node).fd,name);fuse_open_out opened{};opened.fh=nextHandle++;handles[opened.fh]=file;
                out.resize(sizeof(e)+sizeof(opened));memcpy(out.data(),&e,sizeof(e));memcpy(out.data()+sizeof(e),&opened,sizeof(opened));break;}
            case FUSE_OPEN:case FUSE_OPENDIR:{assert(size==sizeof(fuse_open_in));fuse_open_in in{};memcpy(&in,body,sizeof(in));
                assert(!(in.flags&(O_CREAT|O_EXCL|O_TRUNC|O_DIRECTORY|O_NOFOLLOW)));
                std::string path="/proc/self/fd/"+std::to_string(nodes.at(h.node).fd);
                int file=open(path.c_str(),in.flags|O_CLOEXEC|(h.opcode==FUSE_OPENDIR?O_DIRECTORY:0));if(file<0){error=-errno;break;}
                fuse_open_out v{};v.fh=nextHandle++;v.open_flags=(in.flags&O_DIRECT)?FOPEN_DIRECT_IO:0;handles[v.fh]=file;respond(v);break;}
            case FUSE_SETATTR:{assert(size==sizeof(fuse_setattr_in));fuse_setattr_in in{};memcpy(&in,body,sizeof(in));
                assert(in.valid==(FATTR_SIZE|FATTR_FH));if(ftruncate(handles.at(in.fh),in.size)){error=-errno;break;}
                fuse_attr_out v{};v.attr=attr(handles.at(in.fh));respond(v);break;}
            case FUSE_FSYNC:case FUSE_FSYNCDIR:{assert(size==sizeof(fuse_fsync_in));fuse_fsync_in in{};memcpy(&in,body,sizeof(in));assert(!(in.fsync_flags&~1U));if(in.fsync_flags?fdatasync(handles.at(in.fh)):fsync(handles.at(in.fh)))error=-errno;break;}
            case FUSE_FLUSH:{assert(size==sizeof(fuse_flush_in));fuse_flush_in in{};memcpy(&in,body,sizeof(in));assert(handles.count(in.fh));break;}
            case FUSE_READ:{assert(size==sizeof(fuse_read_in));fuse_read_in in{};memcpy(&in,body,sizeof(in));
                out.resize(in.size);auto n=pread(handles.at(in.fh),out.data(),out.size(),in.offset);assert(n>=0);out.resize(n);break;}
            case FUSE_WRITE:{fuse_write_in in{};assert(size>=sizeof(in));memcpy(&in,body,sizeof(in));assert(size==sizeof(in)+in.size);
                auto n=pwrite(handles.at(in.fh),body+sizeof(in),in.size,in.offset);assert(n>=0);fuse_write_out v{};v.size=n;respond(v);break;}
            case FUSE_READDIRPLUS:{assert(size==sizeof(fuse_read_in));fuse_read_in in{};memcpy(&in,body,sizeof(in));
                int copy=dup(handles.at(in.fh));assert(copy>=0);DIR* directory=fdopendir(copy);assert(directory);seekdir(directory,in.offset);
                while(auto d=readdir(directory)) {
                    if(!strcmp(d->d_name,".") || !strcmp(d->d_name,".."))continue;
                    size_t length=strlen(d->d_name),fixed=offsetof(fuse_direntplus,dirent.name),bytes=(fixed+length+7)&~size_t(7);
                    if(out.size()+bytes>in.size)break;
                    auto e=entry(nodes.at(h.node).fd,d->d_name);fuse_dirent item{};item.ino=e.attr.ino;item.off=telldir(directory);item.namelen=length;item.type=d->d_type;
                    if(badDirectory){item.off=0;badDirectory=false;}
                    auto start=out.size();out.resize(start+bytes);memcpy(out.data()+start,&e,sizeof(e));memcpy(out.data()+start+sizeof(e),&item,sizeof(item));memcpy(out.data()+start+fixed,d->d_name,length);
                }
                closedir(directory);break;}
            case FUSE_RENAME:case FUSE_RENAME2:{size_t fixed=h.opcode==FUSE_RENAME?sizeof(fuse_rename_in):sizeof(fuse_rename2_in);assert(size>fixed);
                fuse_rename2_in in{};memcpy(&in,body,fixed);const char* from=body+fixed;size_t n=strnlen(from,size-fixed);assert(n<size-fixed);
                const char* to=from+n+1;assert(to<body+size && memchr(to,0,body+size-to)==body+size-1);
                if(in.flags){error=-EINVAL;break;}
                if(renameat(nodes.at(h.node).fd,from,nodes.at(in.newdir).fd,to))error=-errno;
                break;}
            case FUSE_UNLINK:case FUSE_RMDIR:
                if(requireVfsTypeChecks){struct stat st{};if(!fstatat(nodes.at(h.node).fd,exactName(),&st,AT_SYMLINK_NOFOLLOW) && S_ISDIR(st.st_mode)!=(h.opcode==FUSE_RMDIR)){error=-ENOENT;break;}}
                if(unlinkat(nodes.at(h.node).fd,exactName(),h.opcode==FUSE_RMDIR?AT_REMOVEDIR:0))error=-errno;
                break;
            case FUSE_RELEASE:case FUSE_RELEASEDIR:{assert(size==sizeof(fuse_release_in));fuse_release_in in{};memcpy(&in,body,sizeof(in));
                assert(close(handles.at(in.fh))==0);handles.erase(in.fh);releases++;break;}
            case FUSE_FORGET:{assert(size==sizeof(fuse_forget_in));fuse_forget_in in{};memcpy(&in,body,sizeof(in));
                assert(in.nlookup==1 && nodes.at(h.node).references>0);nodes.at(h.node).references--;forgets++;continue;}
            default:assert(false);
            }
            ResponseHeader header{static_cast<uint32_t>(sizeof(ResponseHeader)+out.size()),error,h.unique};
            std::vector<char> reply(sizeof(header)+out.size());memcpy(reply.data(),&header,sizeof(header));
            if(!out.empty())memcpy(reply.data()+sizeof(header),out.data(),out.size());
            assert(send(fd,reply.data(),reply.size(),MSG_NOSIGNAL)==static_cast<ssize_t>(reply.size()));
        }
        close(fd);
    }
};
