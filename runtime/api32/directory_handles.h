// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fuse_files.h"
#include "../loader/external_protocol.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#ifndef SO_COOKIE
#define SO_COOKIE 57
#endif

namespace pocket::fuse32 {
// Rootless directory descriptors are explicit userspace VFS capabilities.
// Their socket endpoints carry no file data. The other endpoint observes the
// actual final kernel reference across dup/fork/SCM_RIGHTS, without guessing
// process FD tables or counting coalesced inotify events. All directory data
// and attributes come from the live FUSE provider; the loader supplies the
// directory syscall view. This is not a kernel directory/mount or UID boundary.
class DirectoryHandles final {
    struct Entry {
        std::mutex operations;
        Files::Opened cap{};
        Identity opener{};
        int life=-1;
        uint64_t cookie=0,device=0,inode=0,offset=0;
        uint32_t flags=0;
        bool retired=false;
        ~Entry(){if(life>=0)close(life);}
    };
    Files& files_;
    std::mutex mutex_;
    std::map<uint64_t,std::shared_ptr<Entry>> entries_;
    std::atomic<bool> stopping_{false};
    std::thread watching_;
    std::shared_ptr<Entry> entry(int fd) {
        ExternalKernelScope kernel;if(!kernel.okay())return nullptr;
        uint64_t cookie=0;if(identity(fd,&cookie)<0)return nullptr;
        struct stat st{};if(fstat(fd,&st) || !S_ISSOCK(st.st_mode))return nullptr;
        std::lock_guard<std::mutex> lock(mutex_);auto found=entries_.find(cookie);
        return found!=entries_.end() && found->second->device==st.st_dev && found->second->inode==st.st_ino?found->second:nullptr;
    }
    void retire(const std::shared_ptr<Entry>& value) {
        std::lock_guard<std::mutex> lock(value->operations);if(value->retired)return;value->retired=true;
        if(files_.release(value->cap.slot,value->cap.cookie,value->opener)<0)releaseErrors++;
        {std::lock_guard<std::mutex> map(mutex_);auto found=entries_.find(value->cookie);if(found!=entries_.end() && found->second==value)entries_.erase(found);}
        released++;
    }
    void watch() {
        while(!stopping_) {
            std::vector<std::shared_ptr<Entry>> values;std::vector<pollfd> descriptors;
            {std::lock_guard<std::mutex> lock(mutex_);for(auto& item:entries_){values.push_back(item.second);descriptors.push_back({item.second->life,POLLIN,0});}}
            int rc=poll(descriptors.data(),descriptors.size(),50);if(rc<0){if(errno==EINTR)continue;watchErrors++;break;}
            for(size_t i=0;i<values.size();i++) {
                if(descriptors[i].revents&(POLLHUP|POLLERR|POLLNVAL))retire(values[i]);
                else if(descriptors[i].revents&POLLIN) {
                    // No application protocol is accepted on the lifetime FD.
                    // A bounded drain avoids spinning if raw host code writes.
                    char discard[256];recv(values[i]->life,discard,sizeof(discard),MSG_DONTWAIT|MSG_TRUNC);
                }
            }
        }
    }
public:
    std::atomic<uint32_t> released{0},releaseErrors{0},watchErrors{0};
    explicit DirectoryHandles(Files& files):files_(files){watching_=std::thread([this]{watch();});}
    DirectoryHandles(const DirectoryHandles&)=delete;
    ~DirectoryHandles(){stop();}
    static int identity(int fd,uint64_t* result) {
        ExternalKernelScope kernel;if(!kernel.okay())return -EACCES;
        sockaddr_un address{};socklen_t size=sizeof(address);
        if(getsockname(fd,reinterpret_cast<sockaddr*>(&address),&size))return -errno;
        size_t tag=0;
        if(size==offsetof(sockaddr_un,sun_path)+1+POCKET_EXTERNAL_DIRECTORY_TAG_SIZE+sizeof(uint64_t) && !memcmp(address.sun_path+1,POCKET_EXTERNAL_DIRECTORY_TAG,POCKET_EXTERNAL_DIRECTORY_TAG_SIZE))tag=POCKET_EXTERNAL_DIRECTORY_TAG_SIZE;
        else if(size==offsetof(sockaddr_un,sun_path)+1+POCKET_EXTERNAL_PATH_TAG_SIZE+sizeof(uint64_t) && !memcmp(address.sun_path+1,POCKET_EXTERNAL_PATH_TAG,POCKET_EXTERNAL_PATH_TAG_SIZE))tag=POCKET_EXTERNAL_PATH_TAG_SIZE;
        if(address.sun_family!=AF_UNIX || !tag || address.sun_path[0])return -EBADF;
        uint64_t declared=0,cookie=0;memcpy(&declared,address.sun_path+1+tag,sizeof(declared));socklen_t length=sizeof(cookie);
        if(getsockopt(fd,SOL_SOCKET,SO_COOKIE,&cookie,&length))return -errno;
        if(length!=sizeof(cookie) || !cookie || cookie!=declared)return -EBADF;
        *result=cookie;return 0;
    }
    // Success transfers the provider cap to this registry. Failure leaves it
    // with the caller. The returned FD is the sole initial application ref.
    int adopt(const Files::Opened& cap,uint32_t flags,Identity actor) {
        ExternalKernelScope kernel;if(!kernel.okay())return -EACCES;
        if(cap.pathOnly)flags&=O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC;
        else if(!cap.directory || (flags&O_ACCMODE)!=O_RDONLY || (flags&(O_CREAT|O_TRUNC|O_DIRECT|O_PATH)))return -EINVAL;
        auto value=std::make_shared<Entry>();value->cap=cap;value->opener=actor;value->flags=flags&~O_CLOEXEC;
        int pair[2];if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair))return -errno;
        value->life=pair[1];int application=pair[0];
        auto fail=[&](int error){close(application);return error;};
        socklen_t length=sizeof(value->cookie);
        if(getsockopt(application,SOL_SOCKET,SO_COOKIE,&value->cookie,&length))return fail(-errno);
        if(length!=sizeof(value->cookie) || !value->cookie)return fail(-EPROTO);
        sockaddr_un address{};address.sun_family=AF_UNIX;
        size_t tag=cap.pathOnly?POCKET_EXTERNAL_PATH_TAG_SIZE:POCKET_EXTERNAL_DIRECTORY_TAG_SIZE;
        memcpy(address.sun_path+1,cap.pathOnly?POCKET_EXTERNAL_PATH_TAG:POCKET_EXTERNAL_DIRECTORY_TAG,tag);
        memcpy(address.sun_path+1+tag,&value->cookie,sizeof(value->cookie));
        if(bind(application,reinterpret_cast<sockaddr*>(&address),offsetof(sockaddr_un,sun_path)+1+tag+sizeof(value->cookie)))return fail(-errno);
        struct stat st{};if(fstat(application,&st))return fail(-errno);value->device=st.st_dev;value->inode=st.st_ino;
        std::lock_guard<std::mutex> lock(mutex_);if(stopping_)return fail(-ECANCELED);
        if(entries_.size()>=4096 || !entries_.emplace(value->cookie,value).second)return fail(-EMFILE);
        return application;
    }
    int capability(int fd,Files::Opened* result) {
        if(!result)return -EINVAL;
        auto value=entry(fd);if(!value)return -EBADF;
        std::lock_guard<std::mutex> lock(value->operations);if(value->retired)return -EBADF;*result=value->cap;return 0;
    }
    int attributes(int fd,Identity actor,fuse_attr* result) {
        auto value=entry(fd);if(!value)return -EBADF;
        std::lock_guard<std::mutex> lock(value->operations);if(value->retired)return -EBADF;
        int rc=files_.attributes(value->cap.slot,value->cap.cookie,actor,result);return rc<0?rc:value->cap.pathOnly || S_ISDIR(result->mode)?0:-EPROTO;
    }
    int seek(int fd,int64_t offset,uint32_t whence,Identity actor,uint64_t* result) {
        if(!result || (whence!=SEEK_SET && whence!=SEEK_CUR))return -EINVAL;
        auto value=entry(fd);if(!value)return -EBADF;std::lock_guard<std::mutex> lock(value->operations);if(value->retired)return -EBADF;
        if(value->cap.pathOnly)return -EBADF;
        int64_t base=whence==SEEK_CUR?static_cast<int64_t>(value->offset):0;
        if((offset>0 && base>INT64_MAX-offset) || (offset<0 && offset<-base))return -EINVAL;
        uint64_t target=base+offset;
        if(!target && whence==SEEK_SET){int rc=files_.rewindDirectory(value->cap.slot,value->cap.cookie,actor);if(rc<0)return rc;}
        *result=value->offset=target;return 0;
    }
    int flags(int fd,uint64_t* result,bool setting=false,uint32_t flags=0) {
        if(!result)return -EINVAL;
        auto value=entry(fd);if(!value)return -EBADF;
        std::lock_guard<std::mutex> lock(value->operations);if(value->retired)return -EBADF;
        if(setting && value->cap.pathOnly)return -EBADF;
        if(setting){constexpr uint32_t mutableFlags=O_APPEND|O_NONBLOCK|O_ASYNC;value->flags=(value->flags&~mutableFlags)|(flags&mutableFlags);*result=0;}
        else *result=value->flags;
        return 0;
    }
    int sync(int fd,uint32_t flags,Identity actor) {
        auto value=entry(fd);if(!value)return -EBADF;
        std::lock_guard<std::mutex> lock(value->operations);if(value->retired)return -EBADF;
        return files_.sync(value->cap.slot,value->cap.cookie,flags,actor);
    }
    // Hold the shared cursor lock through client copy/ack. A killed client,
    // failed copy_to_user, or disconnected transport never consumes entries.
    template<class Deliver> int read(int fd,uint32_t size,Identity actor,Deliver deliver) {
        if(size<24 || size>65536)return -EINVAL;
        auto value=entry(fd);if(!value)return -EBADF;std::lock_guard<std::mutex> lock(value->operations);if(value->retired)return -EBADF;
        std::vector<Files::DirectoryEntry> entries;int rc=files_.list(value->cap.slot,value->cap.cookie,value->offset,65536,actor,&entries);if(rc<0)return rc;
        std::vector<char> bytes;uint64_t offset=value->offset;
        for(auto& item:entries) {
            const size_t length=(19+item.name.size()+1+7)&~size_t(7);
            if(length>size-bytes.size()){if(bytes.empty())return -EINVAL;break;}
            size_t at=bytes.size();bytes.resize(at+length,0);uint16_t record=length;uint8_t type=(item.attributes.mode&S_IFMT)>>12;
            memcpy(bytes.data()+at,&item.attributes.ino,8);memcpy(bytes.data()+at+8,&item.offset,8);memcpy(bytes.data()+at+16,&record,2);memcpy(bytes.data()+at+18,&type,1);memcpy(bytes.data()+at+19,item.name.c_str(),item.name.size()+1);offset=item.offset;
        }
        rc=deliver(bytes,offset);if(!rc)value->offset=offset;return rc;
    }
    size_t count(){std::lock_guard<std::mutex> lock(mutex_);return entries_.size();}
    void stop() {
        stopping_=true;if(watching_.joinable())watching_.join();
        for(;;){std::shared_ptr<Entry> value;{std::lock_guard<std::mutex> lock(mutex_);if(entries_.empty())break;value=entries_.begin()->second;}retire(value);}
    }
};
static_assert(sizeof(PocketExternalDirectoryReply)==16);
}
