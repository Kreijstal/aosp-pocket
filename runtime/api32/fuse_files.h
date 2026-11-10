// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fuse_namespace.h"
#include "external_kernel.h"
#include <linux/fuse.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <algorithm>
#include <limits>
#include <shared_mutex>
#include <string>

namespace pocket::fuse32 {
// Typed operations for the trusted external-storage broker. Native FUSE inode
// and fh values never cross the client RPC boundary: clients see slot/cookie.
class Files final {
public:
    struct Opened {uint32_t slot,flags;uint64_t cookie;bool directory;bool pathOnly=false;};
private:
    struct References {
        Channel& channel;Identity actor;std::vector<uint64_t> nodes;
        std::shared_ptr<References> parent;
        References(Channel& c,Identity a):channel(c),actor(a){}
        int clear(){int error=0;for(auto node:nodes){int rc=channel.forget(node,1,actor);if(rc<0 && !error)error=rc;}nodes.clear();return error;}
        ~References(){clear();}
    };
    struct File {
        std::shared_mutex operations;
        uint64_t node=0,handle=0,cookie=0,proxyDevice=0,proxyInode=0;
        uint32_t flags=0,openFlags=0;
        Identity opener{};
        std::shared_ptr<References> references;
        bool closed=false,directory=false,pathOnly=false;
        int anchor=-1;
        uint64_t publicInode=0;
        ~File(){if(anchor>=0)close(anchor);}
    };
    Channel& channel_;
    Namespace namespace_;
    const std::string lowerRoot_;
    std::mutex mutex_;
    std::map<uint32_t,std::shared_ptr<File>> files_;
    uint32_t next_=3,maxWrite_=4096;
    uint64_t cookie_=0;
    std::atomic<int> initialized_{0};
    std::atomic<bool> noFlush_{false},noSync_{false},noDirectorySync_{false};
    bool readdirPlus_=false;
    static constexpr size_t MaxTransfer=131072;
    std::shared_ptr<File> file(uint32_t slot,uint64_t cookie) {
        std::lock_guard<std::mutex> lock(mutex_);auto i=files_.find(slot);
        return i!=files_.end() && i->second->cookie==cookie?i->second:nullptr;
    }
    int publish(const std::shared_ptr<File>& current,Opened* result) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(files_.size()>=4096 || next_>INT32_MAX || cookie_==UINT64_MAX)return -EMFILE;
        uint32_t slot=next_++;current->cookie=++cookie_;files_[slot]=current;
        *result={slot,current->openFlags,current->cookie,current->directory,current->pathOnly};return 0;
    }
    int anchorPath(const File& value,std::string* result) {
        struct stat st{};if(fstat(value.anchor,&st))return -errno;
        if(st.st_ino!=value.publicInode)return -ESTALE;
        char name[64],target[4096];snprintf(name,sizeof(name),"/proc/self/fd/%d",value.anchor);
        ssize_t count=readlink(name,target,sizeof(target)-1);if(count<0)return -errno;
        if(count>=static_cast<ssize_t>(sizeof(target)-1))return -ENAMETOOLONG;
        target[count]=0;
        size_t prefix=lowerRoot_.size();
        if(static_cast<size_t>(count)<prefix || memcmp(target,lowerRoot_.data(),prefix) || (target[prefix] && target[prefix]!='/'))return -EXDEV;
        *result=target+prefix;return 0;
    }
    struct Base {uint64_t node=FUSE_ROOT_ID;std::shared_ptr<References> references;int anchor=-1;~Base(){if(anchor>=0)close(anchor);}};
    int base(const Opened* directory,Base* result) {
        if(!directory)return 0;
        auto f=file(directory->slot,directory->cookie);if(!f)return -EBADF;
        std::shared_lock<std::shared_mutex> lock(f->operations);if(f->closed)return -EBADF;
        if(!f->directory)return -ENOTDIR;
        result->node=f->node;result->references=f->references;
        if(f->anchor>=0){result->anchor=fcntl(f->anchor,F_DUPFD_CLOEXEC,0);if(result->anchor<0)return -errno;}
        return 0;
    }
    int anchorNode(int anchor,Namespace::Path& resolved) {
        char fd[64],target[4096];snprintf(fd,sizeof(fd),"/proc/self/fd/%d",anchor);
        ssize_t n=readlink(fd,target,sizeof(target)-1);if(n<0)return -errno;
        if(n>=static_cast<ssize_t>(sizeof(target)-1))return -ENAMETOOLONG;
        target[n]=0;size_t prefix=lowerRoot_.size();
        if(static_cast<size_t>(n)<prefix || memcmp(target,lowerRoot_.data(),prefix) || (target[prefix] && target[prefix]!='/'))return -EXDEV;
        struct stat st{};if(fstat(anchor,&st))return -errno;
        if(!st.st_nlink)return -ENOENT;
        if(!S_ISDIR(st.st_mode))return -ENOTDIR;
        int rc=namespace_.resolve(target[prefix]?target+prefix:"/",resolved);if(rc<0)return rc;
        return st.st_ino==resolved.attr.ino && S_ISDIR(resolved.attr.mode)?0:-ESTALE;
    }
    // FUSE LOOKUP("..") is not a kernel VFS parent operation. In particular,
    // forwarding an unnormalized ".." into MediaProvider would apply its
    // package policy to a misleading textual path. Walk to the last parent
    // component using private kernel anchors, authorizing every ordinary
    // component through the provider. Resolve each actual parent back into
    // provider nodes and compare inode/type before using it as the new base.
    // Anchors follow lower-tree renames; no remembered pathname/parent cache
    // can redirect a held directory to a replacement inode.
    int parents(std::string* path,Base* base,Identity caller) {
        if(initialized_!=2)return -ENODEV;
        size_t last=std::string::npos;
        if(path->empty() || (*path)[0]!='/' || path->size()>4095 || path->find('\0')!=std::string::npos)return -EINVAL;
        for(size_t at=1;at<path->size();) {
            size_t end=path->find('/',at);if(end==std::string::npos)end=path->size();
            if(path->compare(at,end-at,"..")==0)last=end;
            at=end+1;
        }
        if(last==std::string::npos)return 0;
        if(lowerRoot_.empty())return -EOPNOTSUPP;
        ExternalKernelScope kernel;if(!kernel.okay())return -EACCES;
        if(base->anchor<0) {
            base->anchor=::open(lowerRoot_.c_str(),O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(base->anchor<0)return -errno;
        }
        auto references=std::make_shared<References>(channel_,caller);
        references->parent=base->references;base->references=references;
        for(size_t at=1;at<last;) {
            size_t end=path->find('/',at);if(end==std::string::npos)end=path->size();
            std::string name=path->substr(at,end-at);at=end+1;
            if(name.empty())continue;
            Namespace::Path resolved(channel_,caller);int next=-1,rc=0;
            if(name=="..") {
                // A transferred carrier is not permission to bypass the
                // current directory's private-path search policy.
                Namespace::Path current(channel_,caller);
                rc=anchorNode(base->anchor,current);if(rc<0)return rc;
                // Clamp the broker's mount root. Leaving this virtual mount
                // for its guest parent is a separate loader path operation.
                struct stat here{},root{};
                if(fstat(base->anchor,&here) || ::stat(lowerRoot_.c_str(),&root))return -errno;
                bool atRoot=here.st_dev==root.st_dev && here.st_ino==root.st_ino;
                next=openat(base->anchor,atRoot?".":"..",O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
                if(next<0)return -errno;
                rc=anchorNode(next,resolved);
            }else {
                resolved.node=base->node;rc=namespace_.lookup(resolved,name);
                if(!rc && !S_ISDIR(resolved.attr.mode))rc=-ENOTDIR;
                if(!rc){next=openat(base->anchor,name.c_str(),O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(next<0)rc=-errno;}
            }
            if(rc<0){if(next>=0)close(next);return rc;}
            struct stat st{};
            if(fstat(next,&st)){rc=-errno;close(next);return rc;}
            if(st.st_ino!=resolved.attr.ino || !S_ISDIR(st.st_mode) || !S_ISDIR(resolved.attr.mode)){close(next);return -ESTALE;}
            close(base->anchor);base->anchor=next;base->node=resolved.node;
            references->nodes.insert(references->nodes.end(),resolved.references.begin(),resolved.references.end());
            resolved.references.clear();
        }
        *path=last<path->size()?path->substr(last):"/";return 0;
    }
    int anchor(const std::string& path,const Base& parent,const fuse_attr& attr) {
        if(lowerRoot_.empty())return -EOPNOTSUPP;
        ExternalKernelScope kernel;if(!kernel.okay())return -EACCES;
        int fd=parent.anchor>=0?fcntl(parent.anchor,F_DUPFD_CLOEXEC,0): ::open(lowerRoot_.c_str(),O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -errno;
        // Walk private kernel anchors one component at a time. A concurrent
        // lower-tree replacement must not introduce an intermediate symlink
        // after the provider has authorized a different path.
        for(size_t at=1;at<path.size();) {
            size_t end=path.find('/',at);if(end==std::string::npos)end=path.size();std::string name=path.substr(at,end-at);at=end+1;
            if(name.empty() || name==".")continue;
            struct stat parentStat{};if(fstat(fd,&parentStat)){int error=-errno;close(fd);return error;}
            if(!S_ISDIR(parentStat.st_mode)){close(fd);return S_ISLNK(parentStat.st_mode)?-ELOOP:-ENOTDIR;}
            int next=openat(fd,name.c_str(),O_PATH|O_NOFOLLOW|O_CLOEXEC);int error=-errno;close(fd);if(next<0)return error;fd=next;
        }
        struct stat st{};if(fstat(fd,&st)){int result=-errno;close(fd);return result;}
        if(st.st_ino!=attr.ino || (st.st_mode&S_IFMT)!=(attr.mode&S_IFMT)){close(fd);return -ESTALE;}
        return fd;
    }
    int anchorAttributes(const File& file,fuse_attr* result) {
        if(!result)return -EINVAL;
        ExternalKernelScope kernel;if(!kernel.okay())return -EACCES;
        struct stat st{};if(fstat(file.anchor,&st))return -errno;
        if(st.st_ino!=file.publicInode || (file.directory?!S_ISDIR(st.st_mode):!S_ISREG(st.st_mode)))return -ESTALE;
        fuse_attr value{};value.ino=st.st_ino;value.size=st.st_size;value.blocks=st.st_blocks;
        value.atime=st.st_atim.tv_sec;value.mtime=st.st_mtim.tv_sec;value.ctime=st.st_ctim.tv_sec;
        value.atimensec=st.st_atim.tv_nsec;value.mtimensec=st.st_mtim.tv_nsec;value.ctimensec=st.st_ctim.tv_nsec;
        value.mode=st.st_mode;value.nlink=st.st_nlink;value.uid=st.st_uid;value.gid=st.st_gid;value.rdev=st.st_rdev;value.blksize=st.st_blksize;
        *result=value;return 0;
    }
    template<class T> int call(uint32_t opcode,uint64_t node,Identity caller,const T& body,std::vector<char>* out) {
        return channel_.call(opcode,node,caller,&body,sizeof(body),out);
    }
public:
    explicit Files(Channel& channel,const std::string& lowerRoot=""):channel_(channel),namespace_(channel),lowerRoot_(lowerRoot){}
    Files(const Files&)=delete;
    ~Files(){closeAll();}
    int initialize(Identity caller) {
        int expected=0;if(!initialized_.compare_exchange_strong(expected,1))return -EALREADY;
        struct {uint32_t major,minor,readahead,flags;} init{7,31,0,FUSE_DO_READDIRPLUS};std::vector<char> out;
        int rc=call(FUSE_INIT,0,caller,init,&out);if(rc<0)return rc;
        // Through max_write, compatible with the negotiated7.31 reply.
        if(out.size()<24)return -EPROTO;
        uint32_t fields[6];memcpy(fields,out.data(),sizeof(fields));
        if(fields[0]!=7 || fields[1]!=31 || !fields[5])return -EPROTO;
        readdirPlus_=fields[3]&FUSE_DO_READDIRPLUS;
        maxWrite_=std::min<uint32_t>(fields[5],Channel::MaxBody-sizeof(fuse_write_in));initialized_=2;return 0;
    }
    int open(std::string path,uint32_t flags,Identity caller,Opened* result,uint32_t mode=0600,uint32_t mask=0077,const Opened* directory=nullptr) {
        if(flags&O_PATH){flags&=O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC;mode=0;mask=0;}
        if(!result || mode&~07777U || mask&~0777U)return -EINVAL;
        if(initialized_!=2)return -ENODEV;
        if(flags&~static_cast<uint32_t>(O_ACCMODE|O_CLOEXEC|O_LARGEFILE|O_DIRECT|O_CREAT|O_EXCL|O_TRUNC|O_DIRECTORY|O_NOFOLLOW|O_PATH) || (flags&O_ACCMODE)==O_ACCMODE)return -EINVAL;
        if((flags&O_EXCL) && !(flags&O_CREAT))return -EINVAL;
        if((flags&O_TRUNC) && (flags&O_ACCMODE)==O_RDONLY)return -EINVAL;
        if((flags&O_DIRECTORY) && (flags&(O_CREAT|O_TRUNC)))return -EINVAL;
        if((flags&O_CREAT) && path.size()>1 && path.back()=='/')return -EISDIR;
        auto current=std::make_shared<File>();current->opener=caller;
        current->pathOnly=flags&O_PATH;
        current->flags=flags&~(O_CLOEXEC|O_CREAT|O_EXCL|O_TRUNC|O_DIRECTORY|O_NOFOLLOW);
        Base parent;int rc=base(directory,&parent);if(rc<0)return rc;
        rc=parents(&path,&parent,caller);if(rc<0)return rc;
        Namespace::Path resolved(channel_,caller);resolved.node=parent.node;std::vector<char> out;fuse_open_out opened{};
        bool created=false;
        if(flags&O_CREAT) {
            std::string leaf;rc=namespace_.parent(path,resolved,&leaf);if(rc<0)return rc;
            uint64_t parent=resolved.node;
            rc=namespace_.lookup(resolved,leaf);
            if(!rc && (flags&O_EXCL))return -EEXIST;
            if(rc==-ENOENT) {
                fuse_create_in input{};input.flags=flags&~O_CLOEXEC;input.mode=(mode&~mask)|S_IFREG;input.umask=mask;
                std::vector<char> body(sizeof(input)+leaf.size()+1);memcpy(body.data(),&input,sizeof(input));memcpy(body.data()+sizeof(input),leaf.c_str(),leaf.size()+1);
                rc=channel_.call(FUSE_CREATE,parent,caller,body.data(),body.size(),&out);
                if(rc==-EEXIST && !(flags&O_EXCL))rc=namespace_.lookup(resolved,leaf);
                else if(!rc) {
                    if(out.size()!=sizeof(fuse_entry_out)+sizeof(opened))return -EPROTO;
                    fuse_entry_out entry{};memcpy(&entry,out.data(),sizeof(entry));if(!entry.nodeid)return -EPROTO;
                    resolved.node=entry.nodeid;resolved.attr=entry.attr;resolved.references.push_back(entry.nodeid);
                    memcpy(&opened,out.data()+sizeof(entry),sizeof(opened));created=true;
                }
            }
        }else rc=namespace_.resolve(path,resolved);
        if(rc<0)return rc;
        // O_PATH has no provider OPEN call. GETATTR is also a visibility
        // check in MediaProvider (notably for private application paths), so
        // require it before issuing a metadata-only capability.
        if(current->pathOnly){rc=namespace_.attributes(resolved.node,0,false,caller,&resolved.attr);if(rc<0)return rc;}
        current->directory=S_ISDIR(resolved.attr.mode);
        auto releaseHandle=[&] {
            if(current->pathOnly)return 0;
            fuse_release_in release{};release.fh=opened.fh;release.flags=current->flags;
            return call(current->directory?FUSE_RELEASEDIR:FUSE_RELEASE,resolved.node,caller,release,&out);
        };
        if(created && !S_ISREG(resolved.attr.mode)){releaseHandle();return -EPROTO;}
        if((flags&O_DIRECTORY) && !current->directory)return -ENOTDIR;
        if(current->directory && ((flags&O_ACCMODE)!=O_RDONLY || (flags&(O_DIRECT|O_TRUNC|O_CREAT))))return -EISDIR;
        if(!current->directory && !S_ISREG(resolved.attr.mode))return -EOPNOTSUPP;
        if(!lowerRoot_.empty()) {
            current->anchor=anchor(path,parent,resolved.attr);if(current->anchor<0){if(created)releaseHandle();return current->anchor;}
        }else if(current->pathOnly)return -EOPNOTSUPP;
        current->publicInode=resolved.attr.ino;
        if(!created && !current->pathOnly) {
            fuse_open_in opening{};opening.flags=current->flags&~(O_CREAT|O_EXCL|O_TRUNC|O_NOFOLLOW|O_DIRECTORY);
            rc=call(current->directory?FUSE_OPENDIR:FUSE_OPEN,resolved.node,caller,opening,&out);if(rc<0)return rc;
            if(out.size()!=sizeof(opened))return -EPROTO;
            memcpy(&opened,out.data(),sizeof(opened));
        }
        // AppFuse exposes the same mmap/direct-I/O and cache decisions. Do not
        // silently replace unknown server semantics with a cached file copy.
        if(opened.open_flags&~(FOPEN_DIRECT_IO|FOPEN_KEEP_CACHE)) {
            releaseHandle();return -EOPNOTSUPP;
        }
        if(!created && (flags&O_TRUNC)) {
            // ATOMIC_O_TRUNC is not negotiated. Use the approved provider
            // handle for SETATTR, preserving both permission and error checks.
            fuse_setattr_in truncation{};truncation.valid=FATTR_SIZE|FATTR_FH;truncation.fh=opened.fh;
            rc=call(FUSE_SETATTR,resolved.node,caller,truncation,&out);
            if(!rc && out.size()!=sizeof(fuse_attr_out))rc=-EPROTO;
            if(rc<0){releaseHandle();return rc;}
        }
        current->node=resolved.node;current->handle=opened.fh;current->openFlags=opened.open_flags;
        if(resolved.references.empty() && parent.references)current->references=std::move(parent.references);
        else {current->references=std::make_shared<References>(channel_,caller);current->references->nodes=std::move(resolved.references);current->references->parent=std::move(parent.references);}
        rc=publish(current,result);if(rc<0)releaseHandle();return rc;
    }
    int reopen(const Opened& source,uint32_t flags,Identity caller,Opened* result) {
        if(!result)return -EINVAL;
        if(flags&O_PATH)flags&=O_PATH|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW;
        if(flags&~static_cast<uint32_t>(O_ACCMODE|O_CLOEXEC|O_LARGEFILE|O_DIRECT|O_CREAT|O_EXCL|O_TRUNC|O_DIRECTORY|O_NOFOLLOW|O_PATH) || (flags&O_ACCMODE)==O_ACCMODE)return -EINVAL;
        if(flags&O_NOFOLLOW)return -ELOOP;
        if((flags&(O_CREAT|O_EXCL))==(O_CREAT|O_EXCL))return -EEXIST;
        if((flags&O_TRUNC) && (flags&O_ACCMODE)==O_RDONLY)return -EINVAL;
        auto original=file(source.slot,source.cookie);if(!original)return -EBADF;
        std::shared_lock<std::shared_mutex> lock(original->operations);if(original->closed || original->anchor<0)return -EBADF;
        ExternalKernelScope kernel;if(!kernel.okay())return -EACCES;
        struct stat held{};if(fstat(original->anchor,&held))return -errno;
        if(held.st_ino!=original->publicInode || (original->directory?!S_ISDIR(held.st_mode):!S_ISREG(held.st_mode)))return -ESTALE;
        if((flags&O_DIRECTORY) && !original->directory)return -ENOTDIR;
        if(original->directory && !(flags&O_PATH) && ((flags&O_ACCMODE)!=O_RDONLY || (flags&(O_DIRECT|O_TRUNC|O_CREAT))))return -EISDIR;
        if(!(flags&O_PATH) && held.st_nlink) {
            // Refresh current-caller visibility, OPEN and transforms through
            // ordinary provider lookup. Never reopen a replacement inode or
            // create/truncate an old pathname before checking its identity.
            std::string path;int rc=anchorPath(*original,&path);if(rc<0)return rc;
            if(path.empty())path="/";
            Opened fresh{};rc=open(path,flags&~(O_CREAT|O_EXCL|O_TRUNC),caller,&fresh);if(rc<0)return rc;
            auto value=file(fresh.slot,fresh.cookie);if(!value)return -EBADF;bool same=false;
            {std::shared_lock<std::shared_mutex> guard(value->operations);same=value->publicInode==original->publicInode && value->directory==original->directory;}
            if(!same){release(fresh.slot,fresh.cookie,caller);return -ESTALE;}
            if(flags&O_TRUNC){rc=resize(fresh.slot,fresh.cookie,0,caller);if(rc<0){release(fresh.slot,fresh.cookie,caller);return rc;}}
            *result=fresh;return 0;
        }
        auto current=std::make_shared<File>();current->opener=caller;current->node=original->node;
        current->directory=original->directory;current->pathOnly=flags&O_PATH;current->publicInode=original->publicInode;
        current->flags=flags&~(O_CLOEXEC|O_CREAT|O_EXCL|O_TRUNC|O_DIRECTORY|O_NOFOLLOW);
        current->references=original->references;current->anchor=fcntl(original->anchor,F_DUPFD_CLOEXEC,0);if(current->anchor<0)return -errno;
        // O_PATH carries only already-granted inode metadata. A later data
        // reopen must still pass the current provider's OPEN authorization.
        std::vector<char> out;
        auto releaseHandle=[&]{if(!current->pathOnly){fuse_release_in input{};input.fh=current->handle;input.flags=current->flags;call(current->directory?FUSE_RELEASEDIR:FUSE_RELEASE,current->node,caller,input,&out);}};
        if(!current->pathOnly) {
            fuse_open_in input{};input.flags=current->flags;int rc=call(current->directory?FUSE_OPENDIR:FUSE_OPEN,current->node,caller,input,&out);if(rc<0)return rc;
            if(out.size()!=sizeof(fuse_open_out))return -EPROTO;
            fuse_open_out opened{};memcpy(&opened,out.data(),sizeof(opened));current->handle=opened.fh;current->openFlags=opened.open_flags;
            if(opened.open_flags&~(FOPEN_DIRECT_IO|FOPEN_KEEP_CACHE)){releaseHandle();return -EOPNOTSUPP;}
            if(flags&O_TRUNC){fuse_setattr_in truncate{};truncate.valid=FATTR_SIZE|FATTR_FH;truncate.fh=current->handle;rc=call(FUSE_SETATTR,current->node,caller,truncate,&out);if(!rc && out.size()!=sizeof(fuse_attr_out))rc=-EPROTO;if(rc<0){releaseHandle();return rc;}}
        }
        int rc=publish(current,result);if(rc<0)releaseHandle();return rc;
    }
    int size(uint32_t slot,uint64_t cookie,uint64_t* result) {
        auto f=file(slot,cookie);if(!f || !result)return -EBADF;
        std::shared_lock<std::shared_mutex> lock(f->operations);if(f->closed)return -EBADF;
        if(f->pathOnly)return -EBADF;
        if(f->directory)return -EISDIR;
        if(f->anchor>=0){fuse_attr attr{};int rc=anchorAttributes(*f,&attr);if(!rc)*result=attr.size;return rc;}
        fuse_getattr_in input{};input.getattr_flags=FUSE_GETATTR_FH;input.fh=f->handle;std::vector<char> out;
        int rc=call(FUSE_GETATTR,f->node,f->opener,input,&out);if(rc<0)return rc;
        if(out.size()!=sizeof(fuse_attr_out))return -EPROTO;
        fuse_attr_out value{};memcpy(&value,out.data(),sizeof(value));*result=value.attr.size;return 0;
    }
    int attributes(uint32_t slot,uint64_t cookie,Identity caller,fuse_attr* result) {
        auto f=file(slot,cookie);if(!f)return -EBADF;
        std::shared_lock<std::shared_mutex> lock(f->operations);if(f->closed)return -EBADF;
        // A granted FD retains its inode across lower-tree rename/unlink.
        // MediaProvider GETATTR is pathname-based even when given an fh and
        // can return ENOENT after its own ContentResolver rename. Public path
        // lookups still use live provider policy; held capabilities use the
        // private O_PATH anchor whose inode/type was verified when granted.
        if(f->anchor>=0)return anchorAttributes(*f,result);
        return namespace_.attributes(f->node,f->handle,!f->directory && !f->pathOnly,caller,result);
    }
    int link(uint32_t slot,uint64_t cookie,std::string* result) {
        if(!result)return -EINVAL;
        auto f=file(slot,cookie);if(!f)return -EBADF;
        std::shared_lock<std::shared_mutex> lock(f->operations);if(f->closed || f->anchor<0)return -EBADF;
        ExternalKernelScope kernel;if(!kernel.okay())return -EACCES;
        std::string path;int rc=anchorPath(*f,&path);if(!rc)*result="/storage/emulated"+path;return rc;
    }
    int rewindDirectory(uint32_t slot,uint64_t cookie,Identity caller) {
        auto f=file(slot,cookie);if(!f)return -EBADF;
        std::unique_lock<std::shared_mutex> lock(f->operations);if(f->closed)return -EBADF;
        if(f->pathOnly)return -EBADF;
        if(!f->directory)return -ENOTDIR;
        fuse_open_in input{};input.flags=f->flags;std::vector<char> out;
        int rc=call(FUSE_OPENDIR,f->node,caller,input,&out);if(rc<0)return rc;
        if(out.size()!=sizeof(fuse_open_out))return -EPROTO;
        fuse_open_out opened{};memcpy(&opened,out.data(),sizeof(opened));
        if(opened.open_flags&~(FOPEN_DIRECT_IO|FOPEN_KEEP_CACHE)) {
            fuse_release_in release{};release.fh=opened.fh;release.flags=f->flags;call(FUSE_RELEASEDIR,f->node,caller,release,&out);return -EOPNOTSUPP;
        }
        uint64_t old=f->handle;f->handle=opened.fh;f->openFlags=opened.open_flags;
        fuse_release_in release{};release.fh=old;release.flags=f->flags;
        return call(FUSE_RELEASEDIR,f->node,caller,release,&out);
    }
    // Called only after the authenticated Manager attach reply has proven the
    // kernel proxy FD's device/inode. These numbers are never app arguments.
    int bindProxy(uint32_t slot,uint64_t cookie,uint64_t device,uint64_t inode) {
        if(!device || !inode)return -EINVAL;
        auto f=file(slot,cookie);if(!f)return -EBADF;
        std::unique_lock<std::shared_mutex> lock(f->operations);if(f->closed || f->directory || f->pathOnly)return -EBADF;
        if(f->proxyDevice)return -EALREADY;
        f->proxyDevice=device;f->proxyInode=inode;return 0;
    }
    int proxy(int descriptor,Opened* result) {
        if(!result)return -EINVAL;
        ExternalKernelScope kernel;if(!kernel.okay())return -EACCES;
        struct stat st{};if(fstat(descriptor,&st))return -errno;
        if(!S_ISREG(st.st_mode))return -EBADF;
        std::vector<std::pair<uint32_t,std::shared_ptr<File>>> candidates;
        {std::lock_guard<std::mutex> lock(mutex_);for(auto& item:files_)candidates.push_back(item);}
        for(auto& item:candidates) {
            auto& f=*item.second;std::shared_lock<std::shared_mutex> lock(f.operations);
            if(!f.closed && f.proxyDevice && f.proxyDevice==st.st_dev && f.proxyInode==st.st_ino) {
                *result={item.first,f.openFlags,f.cookie,false};return 0;
            }
        }
        return -EBADF;
    }
    int resize(uint32_t slot,uint64_t cookie,uint64_t length,Identity caller) {
        if(length>INT64_MAX)return -EINVAL;
        auto f=file(slot,cookie);if(!f)return -EBADF;
        std::shared_lock<std::shared_mutex> lock(f->operations);if(f->closed)return -EBADF;
        if(f->pathOnly)return -EBADF;
        if(f->directory)return -EISDIR;
        if((f->flags&O_ACCMODE)==O_RDONLY)return -EINVAL;
        fuse_setattr_in input{};input.valid=FATTR_SIZE|FATTR_FH;input.fh=f->handle;input.size=length;
        std::vector<char> out;int rc=call(FUSE_SETATTR,f->node,caller,input,&out);
        return rc<0?rc:out.size()==sizeof(fuse_attr_out)?0:-EPROTO;
    }
    int truncate(const std::string& path,uint64_t length,Identity caller) {
        if(length>INT64_MAX)return -EINVAL;
        Opened cap{};int rc=open(path,O_WRONLY,caller,&cap);if(rc<0)return rc;
        rc=resize(cap.slot,cap.cookie,length,caller);int closed=release(cap.slot,cap.cookie,caller);
        return rc<0?rc:closed;
    }
    int access(std::string path,uint32_t mask,Identity caller,const Opened* directory=nullptr) {
        Base start;int rc=base(directory,&start);if(!rc)rc=parents(&path,&start,caller);return rc<0?rc:initialized_==2?namespace_.access(path,mask,caller,start.node):-ENODEV;
    }
    int stat(std::string path,Identity caller,fuse_attr* result,const Opened* directory=nullptr) {
        Base start;int rc=base(directory,&start);if(!rc)rc=parents(&path,&start,caller);return rc<0?rc:initialized_==2?namespace_.stat(path,caller,result,start.node):-ENODEV;
    }
    int mkdir(std::string path,uint32_t mode,uint32_t mask,Identity caller,const Opened* directory=nullptr) {
        Base start;int rc=base(directory,&start);if(!rc)rc=parents(&path,&start,caller);return rc<0?rc:initialized_==2?namespace_.mkdir(path,mode,mask,caller,start.node):-ENODEV;
    }
    int remove(std::string path,bool directory,Identity caller,const Opened* directoryCap=nullptr) {
        Base start;int rc=base(directoryCap,&start);if(!rc)rc=parents(&path,&start,caller);return rc<0?rc:initialized_==2?namespace_.remove(path,directory,caller,start.node):-ENODEV;
    }
    int rename(std::string from,std::string to,Identity caller,uint32_t flags=0,const Opened* fromDirectory=nullptr,const Opened* toDirectory=nullptr) {
        Base source,target;int rc=base(fromDirectory,&source);if(!rc)rc=base(toDirectory,&target);
        if(!rc)rc=parents(&from,&source,caller);
        if(!rc)rc=parents(&to,&target,caller);
        return rc<0?rc:initialized_==2?namespace_.rename(from,to,caller,flags,source.node,target.node):-ENODEV;
    }
    struct DirectoryEntry {std::string name;uint64_t offset;fuse_attr attributes;};
    int list(uint32_t slot,uint64_t cookie,uint64_t offset,uint32_t size,Identity caller,std::vector<DirectoryEntry>* result) {
        if(!result || offset>INT64_MAX || size<offsetof(fuse_direntplus,dirent.name)+8 || size>Channel::MaxBody)return -EINVAL;
        result->clear();auto f=file(slot,cookie);if(!f)return -EBADF;
        // MediaProvider retains a cursor for each directory handle. Serialize
        // requests on that handle, preserving the caller's opaque seek cookie.
        std::unique_lock<std::shared_mutex> lock(f->operations);if(f->closed)return -EBADF;
        if(f->pathOnly)return -EBADF;
        if(!f->directory)return -ENOTDIR;
        if(!readdirPlus_)return -EOPNOTSUPP;
        fuse_read_in input{};input.fh=f->handle;input.offset=offset;input.size=size;input.flags=f->flags;
        std::vector<char> out;int rc=call(FUSE_READDIRPLUS,f->node,caller,input,&out);if(rc<0)return rc;
        if(out.size()>size)return -EPROTO;
        Namespace::Path references(channel_,caller);std::vector<DirectoryEntry> entries;
        constexpr size_t fixed=offsetof(fuse_direntplus,dirent.name);
        for(size_t at=0;at<out.size();) {
            if(out.size()-at<fixed){rc=-EPROTO;break;}
            fuse_entry_out entry{};fuse_dirent dir{};
            memcpy(&entry,out.data()+at,sizeof(entry));memcpy(&dir,out.data()+at+sizeof(entry),sizeof(dir));
            if(entry.nodeid)references.references.push_back(entry.nodeid);
            size_t bytes=(fixed+dir.namelen+7)&~size_t(7);
            if(!dir.namelen || dir.namelen>255 || bytes>out.size()-at){rc=-EPROTO;break;}
            std::string name(out.data()+at+fixed,dir.namelen);
            const uint64_t nextOffset=dir.off;
            // Continue over framed records even after a semantic error so
            // every granted lookup reference in the reply is forgotten.
            if(!entry.nodeid || !dir.off || dir.off>INT64_MAX || dir.off==offset || name.find('/')!=std::string::npos || name.find('\0')!=std::string::npos || std::any_of(entries.begin(),entries.end(),[nextOffset](const auto& e){return e.offset==nextOffset;}))rc=-EPROTO;
            entries.push_back({std::move(name),dir.off,entry.attr});at+=bytes;
        }
        if(rc<0)return rc;
        *result=std::move(entries);return 0;
    }
    // A search anchor is directory-only. Reauthorize the current caller on
    // the live provider path before handing an O_PATH duplicate to the app.
    // The calling process, never a broker worker, changes its kernel cwd.
    int searchDirectory(uint32_t slot,uint64_t cookie,Identity caller) {
        auto f=file(slot,cookie);if(!f)return -EBADF;
        std::shared_lock<std::shared_mutex> lock(f->operations);if(f->closed)return -EBADF;
        if(!f->directory)return -ENOTDIR;
        if(f->anchor<0)return -EOPNOTSUPP;
        ExternalKernelScope kernel;if(!kernel.okay())return -EACCES;
        struct stat held{};if(fstat(f->anchor,&held))return -errno;
        if(held.st_ino!=f->publicInode || !S_ISDIR(held.st_mode))return -ESTALE;
        int rc;
        if(!held.st_nlink) {
            // Do not resolve a removed name or grant access from the opener's
            // identity. The retained provider node must authorize this caller.
            rc=namespace_.accessNode(f->node,X_OK,caller);
        }else {
            Namespace::Path current(channel_,caller);rc=anchorNode(f->anchor,current);if(rc<0)return rc;
            rc=namespace_.access("/",X_OK,caller,current.node);
        }
        if(rc<0)return rc;
        int duplicate=fcntl(f->anchor,F_DUPFD_CLOEXEC,0);return duplicate<0?-errno:duplicate;
    }
    // Filesystem capacity is volume metadata, not a file-data capability.
    // The app broker authenticates the request and supplies its kernel-backed
    // proxy inode. No UID or native provider inode comes from the app.
    int filesystem(uint32_t slot,Identity caller,fuse_kstatfs* result) {
        if(!result)return -EINVAL;
        std::shared_ptr<File> f;
        {std::lock_guard<std::mutex> lock(mutex_);auto found=files_.find(slot);if(found!=files_.end())f=found->second;}
        if(!f)return -EBADF;
        std::shared_lock<std::shared_mutex> lock(f->operations);if(f->closed)return -EBADF;
        std::vector<char> out;int rc=channel_.call(FUSE_STATFS,f->node,caller,nullptr,0,&out);if(rc<0)return rc;
        if(out.size()!=sizeof(fuse_statfs_out))return -EPROTO;
        fuse_statfs_out value{};memcpy(&value,out.data(),sizeof(value));
        if(!value.st.bsize || !value.st.frsize || !value.st.namelen || value.st.bfree>value.st.blocks || value.st.bavail>value.st.bfree || value.st.ffree>value.st.files)return -EPROTO;
        *result=value.st;return 0;
    }
    // The caller identity is resolved from the kernel tid by the authenticated
    // host-parent adapter. It is intentionally separate from the open identity.
    int read(uint32_t slot,uint64_t cookie,uint64_t offset,uint32_t size,Identity caller,std::vector<char>* bytes) {
        if(!bytes || size>MaxTransfer || offset>INT64_MAX || size>INT64_MAX-offset)return -EINVAL;
        bytes->clear();auto f=file(slot,cookie);if(!f)return -EBADF;
        std::shared_lock<std::shared_mutex> lock(f->operations);if(f->closed || f->pathOnly || (f->flags&O_ACCMODE)==O_WRONLY)return -EBADF;
        if(f->directory)return -EISDIR;
        while(bytes->size()<size) {
            fuse_read_in input{};input.fh=f->handle;input.offset=offset+bytes->size();input.size=std::min<size_t>(size-bytes->size(),Channel::MaxBody);input.flags=f->flags;
            std::vector<char> out;int rc=call(FUSE_READ,f->node,caller,input,&out);if(rc<0)return bytes->empty()?rc:0;
            if(out.size()>input.size)return -EPROTO;
            bytes->insert(bytes->end(),out.begin(),out.end());if(out.size()<input.size)break;
        }
        return 0;
    }
    int write(uint32_t slot,uint64_t cookie,uint64_t offset,const std::vector<char>& bytes,Identity caller,uint32_t* written) {
        if(!written || bytes.size()>MaxTransfer || offset>INT64_MAX || bytes.size()>INT64_MAX-offset)return -EINVAL;
        *written=0;auto f=file(slot,cookie);if(!f)return -EBADF;
        std::shared_lock<std::shared_mutex> lock(f->operations);if(f->closed)return -EBADF;
        if(f->pathOnly)return -EBADF;
        if(f->directory)return -EISDIR;
        if((f->flags&O_ACCMODE)==O_RDONLY)return -EBADF;
        while(*written<bytes.size()) {
            fuse_write_in input{};input.fh=f->handle;input.offset=offset+*written;input.size=std::min<size_t>(bytes.size()-*written,maxWrite_);input.flags=f->flags;
            std::vector<char> body(sizeof(input)+input.size),out;memcpy(body.data(),&input,sizeof(input));memcpy(body.data()+sizeof(input),bytes.data()+*written,input.size);
            int rc=channel_.call(FUSE_WRITE,f->node,caller,body.data(),body.size(),&out);if(rc<0)return *written?0:rc;
            if(out.size()!=sizeof(fuse_write_out))return -EPROTO;
            fuse_write_out value{};memcpy(&value,out.data(),sizeof(value));if(value.size>input.size)return -EPROTO;
            *written+=value.size;if(value.size<input.size)break;
        }
        return 0;
    }
    int sync(uint32_t slot,uint64_t cookie,uint32_t flags,Identity caller) {
        if(flags&~1U)return -EINVAL;
        auto f=file(slot,cookie);if(!f)return -EBADF;
        std::shared_lock<std::shared_mutex> lock(f->operations);if(f->closed)return -EBADF;
        if(f->pathOnly)return -EBADF;
        if(f->directory?noDirectorySync_.load():noSync_.load())return 0;
        fuse_fsync_in input{};input.fh=f->handle;input.fsync_flags=flags;std::vector<char> out;
        int rc=call(f->directory?FUSE_FSYNCDIR:FUSE_FSYNC,f->node,caller,input,&out);
        // Match fs/fuse/file.c: ENOSYS disables this optional operation for
        // the connection; actual I/O and permission failures still propagate.
        if(rc==-ENOSYS){if(f->directory)noDirectorySync_=true;else noSync_=true;return 0;}return rc;
    }
    int flush(uint32_t slot,uint64_t cookie,Identity caller) {
        auto f=file(slot,cookie);if(!f)return -EBADF;std::shared_lock<std::shared_mutex> lock(f->operations);if(f->closed || f->pathOnly)return -EBADF;
        if(f->directory)return -EISDIR;
        if(noFlush_)return 0;
        fuse_flush_in input{};input.fh=f->handle;std::vector<char> out;int rc=call(FUSE_FLUSH,f->node,caller,input,&out);
        if(rc==-ENOSYS){noFlush_=true;return 0;}return rc;
    }
    int release(uint32_t slot,uint64_t cookie,Identity caller) {
        auto f=file(slot,cookie);if(!f)return -EBADF;
        std::unique_lock<std::shared_mutex> lock(f->operations);if(f->closed)return -EBADF;f->closed=true;
        fuse_release_in input{};input.fh=f->handle;input.flags=f->flags;std::vector<char> out;
        int rc=f->pathOnly?0:call(f->directory?FUSE_RELEASEDIR:FUSE_RELEASE,f->node,caller,input,&out);
        int forgotten=f->references.use_count()==1?f->references->clear():0;f->references.reset();
        {std::lock_guard<std::mutex> map(mutex_);files_.erase(slot);}return rc<0?rc:forgotten;
    }
    int opener(uint32_t slot,uint64_t cookie,Identity* identity) {
        auto f=file(slot,cookie);if(!f || !identity)return -EBADF;*identity=f->opener;return 0;
    }
    size_t count(){std::lock_guard<std::mutex> lock(mutex_);return files_.size();}
    void closeAll() {
        for(;;) {
            uint32_t slot;std::shared_ptr<File> f;
            {std::lock_guard<std::mutex> lock(mutex_);if(files_.empty())return;slot=files_.begin()->first;f=files_.begin()->second;}
            release(slot,f->cookie,f->opener);
        }
    }
};
static_assert(sizeof(fuse_open_in)==8 && sizeof(fuse_open_out)==16 && sizeof(fuse_read_in)==40 && sizeof(fuse_write_in)==40);
static_assert(sizeof(fuse_attr_out)==104 && sizeof(fuse_entry_out)==128);
}
