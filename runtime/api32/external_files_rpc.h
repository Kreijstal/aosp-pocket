// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fuse_files.h"
#include "fuse_peer.h"
#include "received_rights.h"
#include <condition_variable>
#include <deque>
#include <set>
#include <sys/un.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <linux/magic.h>

namespace pocket::fuse32 {
constexpr uint32_t ExternalMagic=0x32465850;
enum class FileOp:uint32_t {Size=1,Read=2,Write=3,Sync=4,Flush=5,Release=6};
struct FileRequest {uint32_t magic,version,op,slot;uint64_t cookie,offset;uint32_t size,tid,flags,reserved;};
struct FileReply {int32_t error;uint32_t size;uint64_t value;uint32_t uid,tid,flags,reserved;};
static_assert(sizeof(FileRequest)==48 && sizeof(FileReply)==32);

// Data-only host RPC. File paths, UIDs and native inode/fh values are not
// accepted. The separate app metadata broker will allocate approved slots.
class HostFiles final {
    Files& files_;
    const pid_t parent_;
    const uid_t hostUid_;
    const uint64_t parentStart_;
    int listener_=-1;
    std::string path_;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::condition_variable available_;
    std::deque<int> waiting_;
    std::set<int> active_;
    std::vector<std::thread> workers_;
    std::thread accepting_;
    static int exact(int fd,void* bytes,size_t count,bool writing) {
        auto p=static_cast<char*>(bytes);size_t at=0;
        while(at<count) {
            ssize_t n=writing?send(fd,p+at,count-at,MSG_NOSIGNAL):recv(fd,p+at,count-at,0);
            if(n<0){if(errno==EINTR)continue;return -errno;}
            if(!n)return -ECONNRESET;
            at+=n;
        }
        return 0;
    }
    int lifetime(int socket) {
        // Android may hide a non-debuggable parent's proc directory from its
        // children. The authenticated parent sends its own open kernel stat
        // file, as in InputControl32. A regular file with copied text is not
        // a lifetime proof; verify the kernel fd link and pinned start time.
        char marker=1,control[CMSG_SPACE(8*sizeof(int))]{};
        iovec part{&marker,1};msghdr message{};message.msg_iov=&part;message.msg_iovlen=1;
        message.msg_control=control;message.msg_controllen=sizeof(control);
        ssize_t count;do{count=recvmsg(socket,&message,MSG_CMSG_CLOEXEC);}while(count<0 && errno==EINTR);
        const int receiveError=count<0?errno:0;std::vector<int> rights;
        int decoded=pocket::decodeRights32(message,count,receiveError,sizeof(control),&rights);
        bool valid=!decoded && count==1 && !marker;
        valid=valid && rights.size()==1;
        if(valid)valid=lifetimeDescriptor(rights[0],parent_,parentStart_);
        for(int fd:rights)close(fd);
        return decoded<0?decoded:valid?0:-EACCES;
    }
    int caller(int fd,const FileRequest& request,Identity* identity,bool* fallback) {
        *fallback=false;
        if(request.tid && forwardedIdentity(fd,request.tid,parent_,hostUid_,identity)==0)return 0;
        // Kernel writeback/release can have no caller; host-side cleanup may
        // originate in the host parent. In these cases use the approved open
        // capability's UID/GID, retaining the original kernel tid (including0).
        if(request.tid) {
            // Signal0 checks live thread-group membership without reading
            // the hidden parent's proc directory or delivering a signal.
            if(syscall(__NR_tgkill,parent_,request.tid,0))return -EACCES;
        }
        int rc=files_.opener(request.slot,request.cookie,identity);if(rc<0)return rc;
        identity->pid=request.tid;*fallback=true;return 0;
    }
    void handle(int fd) {
        FileReply reply{};
        if(hostPeer(fd,parent_,hostUid_)<0 || lifetime(fd)<0) {
            denied++;reply.error=-EACCES;exact(fd,&reply,sizeof(reply),true);return;
        }
        FileRequest request{};int rc=exact(fd,&request,sizeof(request),false);if(rc<0)return;
        if(request.magic!=ExternalMagic || request.version!=1 || request.reserved || request.size>131072 || request.slot<3 || request.tid>INT32_MAX)reply.error=-EINVAL;
        std::vector<char> data;
        if(!reply.error && request.op==static_cast<uint32_t>(FileOp::Write)) {
            data.resize(request.size);rc=exact(fd,data.data(),data.size(),false);if(rc<0)return;
        }
        Identity identity{};bool fallback=false;
        if(!reply.error && request.op!=static_cast<uint32_t>(FileOp::Size))reply.error=caller(fd,request,&identity,&fallback);
        if(!reply.error) {
            reply.uid=identity.uid;reply.tid=identity.pid;reply.flags=fallback?1:0;
            switch(static_cast<FileOp>(request.op)) {
            case FileOp::Size:reply.error=files_.size(request.slot,request.cookie,&reply.value);break;
            case FileOp::Read:reply.error=files_.read(request.slot,request.cookie,request.offset,request.size,identity,&data);if(!reply.error){reply.size=data.size();reads++;}break;
            case FileOp::Write:{uint32_t written=0;reply.error=files_.write(request.slot,request.cookie,request.offset,data,identity,&written);reply.value=written;data.clear();if(!reply.error)writes++;break;}
            case FileOp::Sync:reply.error=files_.sync(request.slot,request.cookie,request.flags,identity);break;
            case FileOp::Flush:reply.error=files_.flush(request.slot,request.cookie,identity);break;
            case FileOp::Release:reply.error=files_.release(request.slot,request.cookie,identity);break;
            default:reply.error=-ENOSYS;break;
            }
        }
        if(reply.error){reply.size=0;data.clear();}
        if(exact(fd,&reply,sizeof(reply),true)==0 && reply.size)exact(fd,data.data(),data.size(),true);
    }
    void work() {
        for(;;) {
            int fd;
            {std::unique_lock<std::mutex> lock(mutex_);available_.wait(lock,[this]{return stopping_ || !waiting_.empty();});
                if(waiting_.empty())return;
                fd=waiting_.front();waiting_.pop_front();active_.insert(fd);}
            handle(fd);
            {std::lock_guard<std::mutex> lock(mutex_);active_.erase(fd);close(fd);}
        }
    }
    void accept() {
        while(!stopping_) {
            pollfd state{listener_,POLLIN,0};int rc=poll(&state,1,100);if(rc<=0)continue;
            int peer=accept4(listener_,nullptr,nullptr,SOCK_CLOEXEC);if(peer<0)continue;
            timeval deadline{15,0};setsockopt(peer,SOL_SOCKET,SO_RCVTIMEO,&deadline,sizeof(deadline));setsockopt(peer,SOL_SOCKET,SO_SNDTIMEO,&deadline,sizeof(deadline));
            std::lock_guard<std::mutex> lock(mutex_);if(waiting_.size()>=128){close(peer);continue;}waiting_.push_back(peer);available_.notify_one();
        }
    }
public:
    std::atomic<uint32_t> reads{0},writes{0},denied{0};
    HostFiles(Files& files,pid_t parent,uid_t hostUid,uint64_t parentStart):files_(files),parent_(parent),hostUid_(hostUid),parentStart_(parentStart){}
    HostFiles(const HostFiles&)=delete;
    ~HostFiles(){stop();}
    int start(const std::string& path) {
        if(listener_>=0 || stopping_ || parent_<=0 || !parentStart_ || path.size()>=sizeof(sockaddr_un::sun_path))return -EINVAL;
        listener_=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(listener_<0)return -errno;
        sockaddr_un address{};address.sun_family=AF_UNIX;memcpy(address.sun_path,path.c_str(),path.size()+1);
        if(bind(listener_,reinterpret_cast<sockaddr*>(&address),sizeof(address))) {int error=errno;close(listener_);listener_=-1;return -error;}
        path_=path;if(listen(listener_,128) || chmod(path.c_str(),0600)){int error=errno;stop();return -error;}
        for(int i=0;i<8;i++)workers_.emplace_back([this]{work();});
        accepting_=std::thread([this]{accept();});return 0;
    }
    void stop() {
        if(listener_<0)return;
        stopping_=true;shutdown(listener_,SHUT_RDWR);
        if(accepting_.joinable())accepting_.join();
        {std::lock_guard<std::mutex> lock(mutex_);for(int fd:waiting_)close(fd);waiting_.clear();for(int fd:active_)shutdown(fd,SHUT_RDWR);}
        available_.notify_all();
        for(auto& worker:workers_)if(worker.joinable())worker.join();
        close(listener_);listener_=-1;if(!path_.empty())unlink(path_.c_str());
    }
};
}
