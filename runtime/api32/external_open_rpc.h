// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "external_files_rpc.h"
#include "directory_handles.h"
#include "../loader/external_protocol.h"

namespace pocket::fuse32 {
static_assert(sizeof(PocketExternalOpen)==32 && sizeof(PocketExternalAttach)==32 && sizeof(PocketExternalReply)==8 && sizeof(PocketExternalLocator)==24 && sizeof(PocketExternalAttachReply)==24 && sizeof(PocketExternalStatfsReply)==72);
static_assert(sizeof(PocketExternalRequest2)==48 && sizeof(PocketExternalAttributes)==88 && sizeof(PocketExternalNamespaceReply)==96);
// App metadata requests carry a path, flags and the originating kernel tid.
// The broker derives the guest identity, opens through MediaProvider, and asks
// the authenticated host adapter for a real AppFuse FD for the approved cap.
class AppFiles final {
    Files& files_;
    DirectoryHandles directories_;
    const pid_t parent_;
    const uid_t hostUid_;
    const uint64_t parentStart_;
    const std::string identityRoot_;
    int listener_=-1;
    std::string socket_,locator_,hostSocket_,devicePath_;
    bool ownsLocator_=false;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::condition_variable available_;
    std::deque<int> waiting_;
    std::set<int> active_,outbound_;
    std::vector<std::thread> workers_;
    std::thread accepting_;
    static int exact(int fd,void* data,size_t size,bool writing) {
        size_t at=0;while(at<size) {
            ssize_t n=writing?send(fd,static_cast<char*>(data)+at,size-at,MSG_NOSIGNAL):recv(fd,static_cast<char*>(data)+at,size-at,0);
            if(n<0){if(errno==EINTR)continue;return -errno;}if(!n)return -ECONNRESET;at+=n;
        }return 0;
    }
    struct Rights {
        std::vector<int> values;
        ~Rights(){for(int fd:values)close(fd);}
    };
    static int header(int fd,PocketExternalOpen* request,Rights* rights) {
        char control[CMSG_SPACE(8*sizeof(int))]{};iovec part{request,sizeof(*request)};msghdr message{};
        message.msg_iov=&part;message.msg_iovlen=1;message.msg_control=control;message.msg_controllen=sizeof(control);
        ssize_t n;do{n=recvmsg(fd,&message,MSG_CMSG_CLOEXEC);}while(n<0 && errno==EINTR);
        int decoded=pocket::decodeRights32(message,n,n<0?errno:0,sizeof(control),&rights->values);
        if(decoded<0)return decoded;
        if(n<static_cast<ssize_t>(sizeof(*request))) {
            int rc=exact(fd,reinterpret_cast<char*>(request)+n,sizeof(*request)-n,false);if(rc<0)return rc;
        }
        return 0;
    }
    static int canonical(std::string* path) {
        if(path->find('\0')!=std::string::npos)return -EINVAL;
        for(const std::string& root:{std::string("/storage/emulated"),std::string("/mnt/user/0/emulated")}) {
            if(*path==root){*path="/";return 0;}
            if(path->size()>root.size() && path->compare(0,root.size(),root)==0 && (*path)[root.size()]=='/') {
                path->erase(0,root.size());return 0;
            }
        }
        return -EINVAL;
    }
    static int sendOpen(int fd,int error,int owned,uint32_t kind=0) {
        PocketExternalReply reply{error,kind};char control[CMSG_SPACE(sizeof(int))]{};iovec part{&reply,sizeof(reply)};msghdr message{};
        message.msg_iov=&part;message.msg_iovlen=1;
        if(owned>=0){message.msg_control=control;message.msg_controllen=sizeof(control);cmsghdr* c=CMSG_FIRSTHDR(&message);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&owned,sizeof(owned));}
        ssize_t n;do{n=sendmsg(fd,&message,MSG_NOSIGNAL);}while(n<0 && errno==EINTR);
        return n<=0?n<0?-errno:-EIO:n<static_cast<ssize_t>(sizeof(reply))?exact(fd,reinterpret_cast<char*>(&reply)+n,sizeof(reply)-n,true):0;
    }
    int descriptor(const Files::Opened& cap,uint32_t flags,bool* transferred,bool namespaceView=false) {
        *transferred=false;
        ExternalKernelScope kernel(namespaceView);if(!kernel.okay())return -EACCES;
        int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -errno;
        {std::lock_guard<std::mutex> lock(mutex_);if(stopping_){close(fd);return -ECANCELED;}outbound_.insert(fd);}
        auto finish=[&](int result){std::lock_guard<std::mutex> lock(mutex_);outbound_.erase(fd);close(fd);return result;};
        timeval deadline{15,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&deadline,sizeof(deadline));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&deadline,sizeof(deadline));
        sockaddr_un address{};address.sun_family=AF_UNIX;memcpy(address.sun_path,hostSocket_.c_str(),hostSocket_.size()+1);
        if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)))return finish(-errno);
        if(hostPeer(fd,parent_,hostUid_)<0)return finish(-EACCES);
        PocketExternalAttach request{POCKET_EXTERNAL_ATTACH_MAGIC,1,cap.slot,cap.flags,cap.cookie,flags&O_ACCMODE,0};
        int rc=exact(fd,&request,sizeof(request),true);if(rc<0)return finish(rc);
        // A complete trusted attach request transfers cleanup responsibility
        // to Manager, including attach failure and an interrupted FD reply.
        *transferred=true;
        PocketExternalAttachReply reply{};char control[CMSG_SPACE(8*sizeof(int))]{};iovec part{&reply,sizeof(reply)};msghdr message{};
        message.msg_iov=&part;message.msg_iovlen=1;message.msg_control=control;message.msg_controllen=sizeof(control);
        ssize_t n;do{n=recvmsg(fd,&message,MSG_CMSG_CLOEXEC);}while(n<0 && errno==EINTR);
        const int receiveError=n<0?errno:0;std::vector<int> rights;
        int decoded=pocket::decodeRights32(message,n,receiveError,sizeof(control),&rights);
        bool valid=decoded==0;
        if(valid && n<static_cast<ssize_t>(sizeof(reply)))valid=exact(fd,reinterpret_cast<char*>(&reply)+n,sizeof(reply)-n,false)==0;
        int result=decoded<0?decoded:-EPROTO;
        bool lifetimeOk=rights.size()==2 && lifetimeDescriptor(rights[1],parent_,parentStart_);
        if(valid && !reply.reserved && reply.error<0 && reply.error>=-4095 && rights.empty())result=reply.error;
        else if(valid && !reply.error && !reply.reserved && rights.size()==2 && lifetimeOk) {
            struct stat st{};
            // Host AppFuseBridge rejects STATFS itself. Authenticate this
            // mount's device and approved inode, as with the keeper FD.
            if(reply.device && reply.node==cap.slot && !fstat(rights[0],&st) && S_ISREG(st.st_mode) && st.st_dev==reply.device && st.st_ino==reply.node) {
                result=files_.bindProxy(cap.slot,cap.cookie,st.st_dev,st.st_ino);
                if(!result){result=rights[0];rights.erase(rights.begin());}
            }
        }
#ifdef POCKET_EXTERNAL_OPEN_TRACE
        fprintf(stderr,"MEDIA_PATH_ATTACH result=%d recv=%zd valid=%d errno=%d rights=%zu lifetime=%d device=%llu inode=%llu\n",result,n,valid,reply.error,rights.size(),lifetimeOk,(unsigned long long)reply.device,(unsigned long long)reply.node);
#endif
        for(int value:rights)close(value);
        return finish(result);
    }
    void statfsRequest(int fd,const PocketExternalRequest2& request,const Rights& rights) {
        const auto& h=request.header;PocketExternalStatfsReply reply{};
        if(h.magic!=POCKET_EXTERNAL_OPEN_MAGIC || h.version!=2 || h.mode!=POCKET_EXT_STATFS ||
            h.flags || h.path_size || h.reserved[0] || h.reserved[1] || !h.tid || h.tid>INT32_MAX ||
            rights.values.size()!=1 || request.permissions || request.umask || request.size)reply.error=-EINVAL;
        Identity actor{};if(!reply.error)reply.error=peerIdentity(fd,h.tid,hostUid_,&actor,identityRoot_.c_str());
        Files::Opened cap{};
        if(!reply.error)reply.error=directories_.capability(rights.values[0],&cap);
        if(!reply.error && !cap.directory && !cap.pathOnly)reply.error=-EBADF;
        // O_PATH supports fstatfs on Linux too. These are volume attributes,
        // not file data; the actual held descriptor proves the capability.
        fuse_kstatfs value{};if(!reply.error)reply.error=files_.filesystem(cap.slot,actor,&value);
        if(!reply.error) {
            reply.blocks=value.blocks;reply.bfree=value.bfree;reply.bavail=value.bavail;
            reply.files=value.files;reply.ffree=value.ffree;reply.bsize=value.bsize;
            reply.namelen=value.namelen;reply.frsize=value.frsize;statQueries++;namespaceQueries++;
        }else denied++;
        exact(fd,&reply,sizeof(reply),true);
    }
    void directoryRequest(int fd,const PocketExternalRequest2& request,const Rights& rights) {
        const auto& h=request.header;PocketExternalDirectoryReply reply{};
        if(h.magic!=POCKET_EXTERNAL_OPEN_MAGIC || h.reserved[0] || h.reserved[1] || h.path_size || !h.tid || h.tid>INT32_MAX || rights.values.size()!=1 || request.permissions || request.umask || ((h.mode==POCKET_EXT_DIR_READ || h.mode==POCKET_EXT_DIR_SYNC || h.mode==POCKET_EXT_FD_LINK) && request.size))reply.error=-EINVAL;
        Identity actor{};if(!reply.error)reply.error=peerIdentity(fd,h.tid,hostUid_,&actor,identityRoot_.c_str());
        bool responded=false;
        if(!reply.error && h.mode==POCKET_EXT_FD_LINK) {
            Files::Opened cap{};uint64_t cookie=0;std::string path;
            reply.error=!h.flags || h.flags>4095?-EINVAL:DirectoryHandles::identity(rights.values[0],&cookie)==0?directories_.capability(rights.values[0],&cap):files_.proxy(rights.values[0],&cap);
            if(!reply.error)reply.error=files_.link(cap.slot,cap.cookie,&path);
            if(!reply.error){reply.size=std::min<size_t>(h.flags,path.size());responded=true;if(!exact(fd,&reply,sizeof(reply),true) && reply.size)exact(fd,path.data(),reply.size,true);}
        }else if(!reply.error && h.mode==POCKET_EXT_DIR_READ) {
            reply.error=directories_.read(rights.values[0],h.flags,actor,[&](const std::vector<char>& data,uint64_t offset) {
                PocketExternalDirectoryReply header{0,static_cast<uint32_t>(data.size()),offset};responded=true;
                int rc=exact(fd,&header,sizeof(header),true);if(!rc && !data.empty())rc=exact(fd,const_cast<char*>(data.data()),data.size(),true);if(rc<0)return rc;
                unsigned char ack=0;rc=exact(fd,&ack,1,false);return rc<0?rc:ack==1?0:ack==0?-EFAULT:-EPROTO;
            });
        }else if(!reply.error && h.mode==POCKET_EXT_DIR_SEEK)reply.error=directories_.seek(rights.values[0],static_cast<int64_t>(request.size),h.flags,actor,&reply.value);
        else if(!reply.error && h.mode==POCKET_EXT_DIR_FLAGS)reply.error=h.flags>1 || (!h.flags && request.size) || request.size>UINT32_MAX?-EINVAL:directories_.flags(rights.values[0],&reply.value,h.flags,request.size);
        else if(!reply.error && h.mode==POCKET_EXT_DIR_SYNC)reply.error=directories_.sync(rights.values[0],h.flags,actor);
        if(reply.error)denied++;else namespaceQueries++;
        if(!responded)exact(fd,&reply,sizeof(reply),true);
    }
    void namespaceRequest(int fd,const PocketExternalOpen& header,const Rights& rights) {
        PocketExternalRequest2 request{};request.header=header;PocketExternalNamespaceReply reply{};
        int rc=exact(fd,reinterpret_cast<char*>(&request)+sizeof(header),sizeof(request)-sizeof(header),false);if(rc<0)return;
        if(header.mode==POCKET_EXT_STATFS){statfsRequest(fd,request,rights);return;}
        if(header.mode==POCKET_EXT_FD_REOPEN) {
            int error=0,owned=-1;Identity actor{};Files::Opened source{},cap{};uint64_t cookie=0;
            if(header.magic!=POCKET_EXTERNAL_OPEN_MAGIC || header.path_size || header.reserved[0] || header.reserved[1] || !header.tid || header.tid>INT32_MAX || rights.values.size()!=1 || request.permissions || request.umask || request.size)error=-EINVAL;
            if(!error)error=peerIdentity(fd,header.tid,hostUid_,&actor,identityRoot_.c_str());
            if(!error)error=DirectoryHandles::identity(rights.values[0],&cookie)==0?directories_.capability(rights.values[0],&source):files_.proxy(rights.values[0],&source);
            if(!error)error=files_.reopen(source,header.flags,actor,&cap);
            if(!error && (cap.directory || cap.pathOnly)) {
                owned=directories_.adopt(cap,header.flags,actor);if(owned<0){error=owned;files_.release(cap.slot,cap.cookie,actor);}
            }else if(!error) {
                bool transferred=false;owned=descriptor(cap,header.flags,&transferred,true);
                if(owned<0){error=owned;if(!transferred)files_.release(cap.slot,cap.cookie,actor);}
            }
            if(error)denied++;else {opened++;namespaceQueries++;}
            sendOpen(fd,error,owned,error?0:cap.pathOnly?POCKET_EXTERNAL_OPEN_PATH:cap.directory?POCKET_EXTERNAL_OPEN_DIRECTORY:0);if(owned>=0)close(owned);return;
        }
        if(header.mode==POCKET_EXT_DIR_SEARCH) {
            int error=0,owned=-1;Identity actor{};Files::Opened cap{};
            if(header.magic!=POCKET_EXTERNAL_OPEN_MAGIC || header.flags || header.path_size || header.reserved[0] || header.reserved[1] || !header.tid || header.tid>INT32_MAX || rights.values.size()!=1 || request.permissions || request.umask || request.size)error=-EINVAL;
            if(!error)error=peerIdentity(fd,header.tid,hostUid_,&actor,identityRoot_.c_str());
            if(!error)error=directories_.capability(rights.values[0],&cap);
            if(!error){owned=files_.searchDirectory(cap.slot,cap.cookie,actor);if(owned<0)error=owned;}
            if(error)denied++;else namespaceQueries++;
            sendOpen(fd,error,owned);if(owned>=0)close(owned);return;
        }
        if(header.mode>=POCKET_EXT_DIR_READ && header.mode<=POCKET_EXT_FD_LINK){directoryRequest(fd,request,rights);return;}
        const bool byFd=header.mode==POCKET_EXT_FSTAT || header.mode==POCKET_EXT_FTRUNCATE;
        const bool creating=header.mode==POCKET_EXT_OPEN || header.mode==POCKET_EXT_MKDIR;
        const unsigned bases=header.reserved[1],count=!!(bases&POCKET_EXTERNAL_AT_FIRST)+!!(bases&POCKET_EXTERNAL_AT_SECOND);
        if(header.magic!=POCKET_EXTERNAL_OPEN_MAGIC || bases&~3U || (byFd && bases) || ((bases&POCKET_EXTERNAL_AT_SECOND) && header.mode!=POCKET_EXT_RENAME) || !header.tid || header.tid>INT32_MAX || header.path_size>4095 || header.reserved[0]>4095
            || rights.values.size()!=(byFd?1U:count) || (byFd?(header.path_size || header.reserved[0]):!header.path_size)
            || (header.mode==POCKET_EXT_RENAME?!header.reserved[0]:header.reserved[0]!=0)
            || (!creating && (request.permissions || request.umask)) || request.permissions&~07777U || request.umask&~0777U
            || (header.mode!=POCKET_EXT_TRUNCATE && header.mode!=POCKET_EXT_FTRUNCATE && request.size))reply.error=-EINVAL;
        Identity actor{};if(!reply.error)reply.error=peerIdentity(fd,header.tid,hostUid_,&actor,identityRoot_.c_str());
        Files::Opened firstBase{},secondBase{};unsigned proof=0;
        if(!reply.error && (bases&POCKET_EXTERNAL_AT_FIRST))reply.error=directories_.capability(rights.values[proof++],&firstBase);
        if(!reply.error && (bases&POCKET_EXTERNAL_AT_SECOND))reply.error=directories_.capability(rights.values[proof++],&secondBase);
        const Files::Opened* first=(bases&POCKET_EXTERNAL_AT_FIRST)?&firstBase:nullptr;
        const Files::Opened* other=(bases&POCKET_EXTERNAL_AT_SECOND)?&secondBase:nullptr;
        auto resolve=[&](std::string* path,bool relative){if(!relative)return canonical(path);
            if(path->empty() || (*path)[0]=='/' || path->find('\0')!=std::string::npos)return -EINVAL;
            if(*path==".")*path="/";else path->insert(0,"/");return 0;};
        std::string path,second;
        if(!reply.error && !byFd) {
            path.resize(header.path_size);if(exact(fd,path.data(),path.size(),false)<0)return;
            reply.error=resolve(&path,first!=nullptr);
            if(!reply.error && header.reserved[0]){second.resize(header.reserved[0]);if(exact(fd,second.data(),second.size(),false)<0)return;reply.error=resolve(&second,other!=nullptr);}
        }
        Files::Opened cap{};fuse_attr attributes{};uint64_t device=0;bool hasAttributes=false;int owned=-1;
        if(!reply.error && byFd) {
            uint64_t cookie=0;
            reply.error=DirectoryHandles::identity(rights.values[0],&cookie)==0?directories_.capability(rights.values[0],&cap):files_.proxy(rights.values[0],&cap);
        }
        if(!reply.error)switch(header.mode) {
        case POCKET_EXT_OPEN:
            reply.error=files_.open(path,header.flags,actor,&cap,request.permissions,request.umask,first);
            if(!reply.error && (cap.directory || cap.pathOnly)) {
                owned=directories_.adopt(cap,header.flags,actor);if(owned<0){reply.error=owned;files_.release(cap.slot,cap.cookie,actor);}else opened++;
            }else if(!reply.error){bool transferred=false;owned=descriptor(cap,header.flags,&transferred,true);if(owned<0){reply.error=owned;if(!transferred)files_.release(cap.slot,cap.cookie,actor);}else opened++;}
            break;
        case POCKET_EXT_STAT:
            if(header.flags&~static_cast<uint32_t>(AT_SYMLINK_NOFOLLOW|AT_NO_AUTOMOUNT)){reply.error=-EINVAL;break;}
            reply.error=files_.stat(path,actor,&attributes,first);hasAttributes=!reply.error;
            if(!reply.error){ssize_t n=readIdentityFile(devicePath_.c_str(),&device,sizeof(device));if(n!=sizeof(device) || !device)reply.error=n<0?n:-ENODEV;}
            break;
        case POCKET_EXT_FSTAT:
            if(header.flags){reply.error=-EINVAL;break;}
            reply.error=cap.directory || cap.pathOnly?directories_.attributes(rights.values[0],actor,&attributes):files_.attributes(cap.slot,cap.cookie,actor,&attributes);hasAttributes=!reply.error;
            if(!reply.error && (cap.directory || cap.pathOnly)){ssize_t n=readIdentityFile(devicePath_.c_str(),&device,sizeof(device));if(n!=sizeof(device) || !device)reply.error=n<0?n:-ENODEV;}
            else if(!reply.error){ExternalKernelScope kernel;struct stat st{};if(!kernel.okay())reply.error=-EACCES;else if(fstat(rights.values[0],&st))reply.error=-errno;else device=st.st_dev;}
            break;
        case POCKET_EXT_MKDIR:reply.error=header.flags?-EINVAL:files_.mkdir(path,request.permissions,request.umask,actor,first);break;
        case POCKET_EXT_REMOVE:reply.error=header.flags&~static_cast<uint32_t>(AT_REMOVEDIR)?-EINVAL:files_.remove(path,header.flags&AT_REMOVEDIR,actor,first);break;
        case POCKET_EXT_RENAME:reply.error=files_.rename(path,second,actor,header.flags,first,other);break;
        case POCKET_EXT_TRUNCATE:reply.error=header.flags?-EINVAL:files_.truncate(path,request.size,actor);break;
        case POCKET_EXT_FTRUNCATE: {
            int flags=fcntl(rights.values[0],F_GETFL);
            reply.error=header.flags?-EINVAL:cap.pathOnly?-EBADF:cap.directory?-EISDIR:flags<0?-errno:(flags&O_ACCMODE)==O_RDONLY?-EINVAL:files_.resize(cap.slot,cap.cookie,request.size,actor);break;
        }
        case POCKET_EXT_ACCESS:reply.error=files_.access(path,header.flags,actor,first);break;
        default:reply.error=-ENOSYS;break;
        }
        if(reply.error)denied++;else namespaceQueries++;
        if(header.mode==POCKET_EXT_OPEN){sendOpen(fd,reply.error,owned,reply.error?0:cap.pathOnly?POCKET_EXTERNAL_OPEN_PATH:cap.directory?POCKET_EXTERNAL_OPEN_DIRECTORY:0);if(owned>=0)close(owned);return;}
        if(!reply.error && hasAttributes)reply.attributes={device,attributes.ino,attributes.size,attributes.blocks,attributes.atime,attributes.mtime,attributes.ctime,attributes.mode,attributes.nlink,attributes.uid,attributes.gid,attributes.blksize,attributes.atimensec,attributes.mtimensec,attributes.ctimensec};
        exact(fd,&reply,sizeof(reply),true);
    }
    void handle(int fd) {
        PocketExternalOpen request{};PocketExternalReply reply{};int owned=-1;
        Rights rights;int rc=header(fd,&request,&rights);
        if(rc<0){if(rc==-EPROTO){if(request.version==2 && request.mode!=POCKET_EXT_OPEN){PocketExternalNamespaceReply invalid{};invalid.error=rc;exact(fd,&invalid,sizeof(invalid),true);}else sendOpen(fd,rc,-1);}return;}
        if(request.version==2){namespaceRequest(fd,request,rights);return;}
        if(!rights.values.empty()){sendOpen(fd,-EINVAL,-1);return;}
        if(request.mode==1) {
            PocketExternalStatfsReply stats{};Identity actor{};
            if(request.magic!=POCKET_EXTERNAL_OPEN_MAGIC || request.version!=1 || request.flags || request.path_size || request.reserved[0]<3 || request.reserved[1] || !request.tid || request.tid>INT32_MAX)stats.error=-EINVAL;
            if(!stats.error)stats.error=peerIdentity(fd,request.tid,hostUid_,&actor,identityRoot_.c_str());
            fuse_kstatfs value{};if(!stats.error)stats.error=files_.filesystem(request.reserved[0],actor,&value);
            if(!stats.error){stats.blocks=value.blocks;stats.bfree=value.bfree;stats.bavail=value.bavail;stats.files=value.files;stats.ffree=value.ffree;stats.bsize=value.bsize;stats.namelen=value.namelen;stats.frsize=value.frsize;statQueries++;}
            exact(fd,&stats,sizeof(stats),true);return;
        }
        if(request.magic!=POCKET_EXTERNAL_OPEN_MAGIC || request.version!=1 || request.mode || request.reserved[0] || request.reserved[1] || !request.path_size || request.path_size>4095 || !request.tid || request.tid>INT32_MAX)reply.error=-EINVAL;
        // Version1 only transports existing regular-file FDs. Creation needs
        // an explicit mode/umask and directory FDs need a directory transport;
        // do not accidentally expose new trusted Files operations via v1.
        if(request.flags&~static_cast<uint32_t>(O_ACCMODE|O_CLOEXEC|O_LARGEFILE|O_DIRECT))reply.error=-EINVAL;
        Identity actor{};
        if(!reply.error)reply.error=peerIdentity(fd,request.tid,hostUid_,&actor,identityRoot_.c_str());
        std::string path;
        if(!reply.error) {
            path.resize(request.path_size);rc=exact(fd,path.data(),path.size(),false);if(rc<0)return;
            // Only these two canonical roots enter this broker. MediaProvider's
            // PASS_THROUGH lower view is selected in the loader, not here.
            const std::string upper="/storage/emulated",forced="/mnt/user/0/emulated";
            if(path.compare(0,upper.size(),upper)==0 && path.size()>upper.size() && path[upper.size()]=='/')path.erase(0,upper.size());
            else if(path.compare(0,forced.size(),forced)==0 && path.size()>forced.size() && path[forced.size()]=='/')path.erase(0,forced.size());
            else reply.error=-EINVAL;
        }
        Files::Opened cap{};
        if(!reply.error) {
            reply.error=files_.open(path,request.flags,actor,&cap);
            if(!reply.error && cap.directory) {
                files_.release(cap.slot,cap.cookie,actor);reply.error=-EISDIR;
            }
            if(!reply.error) {
                bool transferred=false;owned=descriptor(cap,request.flags,&transferred);
                if(owned<0){reply.error=owned;if(!transferred)files_.release(cap.slot,cap.cookie,actor);}
                else opened++;
            }
        }
        if(reply.error)denied++;
#ifdef POCKET_EXTERNAL_OPEN_TRACE
        fprintf(stderr,"MEDIA_PATH_OPEN result=%d uid=%u tid=%u flags=%u transferred_fd=%d\n",reply.error,actor.uid,request.tid,request.flags,owned>=0);
#endif
        char control[CMSG_SPACE(sizeof(int))]{};iovec part{&reply,sizeof(reply)};msghdr message{};message.msg_iov=&part;message.msg_iovlen=1;
        if(owned>=0) {
            message.msg_control=control;message.msg_controllen=sizeof(control);cmsghdr* c=CMSG_FIRSTHDR(&message);
            c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),&owned,sizeof(owned));
        }
        ssize_t n;do{n=sendmsg(fd,&message,MSG_NOSIGNAL);}while(n<0 && errno==EINTR);
        if(n>0 && n<static_cast<ssize_t>(sizeof(reply)))exact(fd,reinterpret_cast<char*>(&reply)+n,sizeof(reply)-n,true);
        if(owned>=0)close(owned);
    }
    void work() {
        for(;;) {
            int fd;{std::unique_lock<std::mutex> lock(mutex_);available_.wait(lock,[&]{return stopping_ || !waiting_.empty();});
                if(waiting_.empty())return;
                fd=waiting_.front();waiting_.pop_front();active_.insert(fd);}
            handle(fd);{std::lock_guard<std::mutex> lock(mutex_);active_.erase(fd);close(fd);}
        }
    }
    void accept() {
        while(!stopping_) {
            pollfd p{listener_,POLLIN,0};if(poll(&p,1,100)<=0)continue;
            int fd=accept4(listener_,nullptr,nullptr,SOCK_CLOEXEC);if(fd<0)continue;
            timeval deadline{15,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&deadline,sizeof(deadline));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&deadline,sizeof(deadline));
            std::lock_guard<std::mutex> lock(mutex_);if(waiting_.size()>=64){close(fd);continue;}waiting_.push_back(fd);available_.notify_one();
        }
    }
public:
    std::atomic<uint32_t> opened{0},denied{0},statQueries{0},namespaceQueries{0};
    AppFiles(Files& files,pid_t parent,uid_t hostUid,uint64_t start,const std::string& identityRoot="/.pocket-identity"):files_(files),directories_(files),parent_(parent),hostUid_(hostUid),parentStart_(start),identityRoot_(identityRoot){}
    AppFiles(const AppFiles&)=delete;
    ~AppFiles(){stop();}
    int start(const std::string& socketPath,const std::string& hostPath,const std::string& locatorPath,const std::string& devicePath="/.pocket-external-device") {
        if(listener_>=0 || stopping_ || parent_<=0 || !parentStart_ || socketPath.size()>=sizeof(sockaddr_un::sun_path) || hostPath.size()>=sizeof(sockaddr_un::sun_path))return -EINVAL;
        hostSocket_=hostPath;devicePath_=devicePath;listener_=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(listener_<0)return -errno;
        sockaddr_un address{};address.sun_family=AF_UNIX;memcpy(address.sun_path,socketPath.c_str(),socketPath.size()+1);
        if(bind(listener_,reinterpret_cast<sockaddr*>(&address),sizeof(address))){int error=errno;close(listener_);listener_=-1;return -error;}
        socket_=socketPath;
        if(listen(listener_,64) || chmod(socketPath.c_str(),0600)){int error=errno;stop();return -error;}
        char stat[4096];ssize_t n=readIdentityFile("/proc/self/stat",stat,sizeof(stat));
        PocketExternalLocator info{POCKET_EXTERNAL_LOCATOR_MAGIC,1,static_cast<uint32_t>(getpid()),0,n>0?pocket_process_start(stat,n):0};
        if(!info.start){stop();return -EACCES;}
        int output=open(locatorPath.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(output<0){int error=errno;stop();return -error;}
        locator_=locatorPath;ownsLocator_=true;n=write(output,&info,sizeof(info));int error=errno;close(output);
        if(n!=sizeof(info)){stop();return n<0?-error:-EIO;}
        for(int i=0;i<4;i++)workers_.emplace_back([this]{work();});
        accepting_=std::thread([this]{accept();});return 0;
    }
    void stop() {
        if(listener_<0)return;
        stopping_=true;
        if(ownsLocator_){unlink(locator_.c_str());ownsLocator_=false;}
        shutdown(listener_,SHUT_RDWR);if(accepting_.joinable())accepting_.join();
        {std::lock_guard<std::mutex> lock(mutex_);for(int fd:waiting_)close(fd);waiting_.clear();for(int fd:active_)shutdown(fd,SHUT_RDWR);for(int fd:outbound_)shutdown(fd,SHUT_RDWR);}
        available_.notify_all();for(auto& worker:workers_)if(worker.joinable())worker.join();
        directories_.stop();
        close(listener_);listener_=-1;unlink(socket_.c_str());
    }
};
}
