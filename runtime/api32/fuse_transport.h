// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace pocket::fuse32 {
// Linux FUSE 7.x wire headers, checked against the pinned12L UAPI by the probe.
struct RequestHeader {
    uint32_t length,opcode;
    uint64_t unique,node;
    uint32_t uid,gid,pid,padding;
};
struct ResponseHeader {uint32_t length;int32_t error;uint64_t unique;};
struct Identity {uint32_t uid,gid,pid;};
static_assert(sizeof(RequestHeader)==40 && sizeof(ResponseHeader)==16);

// Internal trusted broker transport. Identity must come from the broker's
// authenticated process record, never a UID supplied by an app RPC client.
// No inode/data cache is kept, so validated invalidation notifications can be
// discarded. A dedicated reader dispatches out-of-order replies by unique ID;
// slow fsync must not prevent independent reads. FORGET is reply-free.
class Channel final {
    const int fd_;
    const int timeoutMs_;
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    std::timed_mutex sending_;
    std::thread reader_;
    bool failed_=false;
    uint64_t unique_=0;
    using Clock=std::chrono::steady_clock;
    struct Pending {
        std::condition_variable ready;
        bool done=false;
        int error=0;
        std::vector<char> bytes;
    };
    std::map<uint64_t,std::shared_ptr<Pending>> pending_;
    int fail(int error) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            failed_=true;cancelled_=true;
            for(auto& entry:pending_)if(!entry.second->done) {
                auto& p=*entry.second;p.done=true;p.error=-error;p.ready.notify_all();
            }
        }
        shutdown(fd_,SHUT_RDWR);return -error;
    }
    int ready(short events,Clock::time_point deadline) {
        while(true) {
            if(cancelled_)return -ECANCELED;
            auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
            if(left<=0)return -ETIMEDOUT;
            pollfd p{fd_,events,0};int rc=poll(&p,1,static_cast<int>(left));
            if(rc<0){if(errno==EINTR)continue;return -errno;}
            if(!rc)return -ETIMEDOUT;
            if(p.revents&POLLNVAL)return -EBADF;
            if(p.revents&events)return 0;
            if(p.revents&(POLLHUP|POLLERR))return -ECONNRESET;
        }
    }
    int send(uint32_t opcode,uint64_t node,Identity identity,const void* body,size_t size,
             uint64_t id,Clock::time_point deadline) {
        RequestHeader h{static_cast<uint32_t>(sizeof(h)+size),opcode,id,node,identity.uid,identity.gid,identity.pid,0};
        iovec parts[]={{&h,sizeof(h)},{const_cast<void*>(body),size}};
        msghdr message{};message.msg_iov=parts;message.msg_iovlen=size?2:1;
        while(true) {
            int rc=ready(POLLOUT,deadline);if(rc<0)return fail(-rc);
            ssize_t n=sendmsg(fd_,&message,MSG_NOSIGNAL|MSG_DONTWAIT);
            if(n<0){if(errno==EINTR || errno==EAGAIN)continue;return fail(errno);}
            return n==h.length?0:fail(EIO);
        }
    }
    static bool invalidation(const ResponseHeader& h,const char* bytes,size_t size) {
        // FUSE_NOTIFY_INVAL_INODE, INVAL_ENTRY, DELETE. All other unsolicited
        // messages require an implementation before they can be accepted.
        if(h.error==2)return size==24;
        size_t fixed=h.error==3?16:h.error==6?24:0;
        if(!fixed || size<fixed+1)return false;
        uint32_t length;std::memcpy(&length,bytes+fixed-8,sizeof(length));
        return length<=255 && size==fixed+length+1 && bytes[fixed+length]==0
            && std::memchr(bytes+fixed,0,length)==nullptr;
    }
    void receive() {
        std::vector<char> packet(MaxPacket);unsigned notifications=0;
        while(!cancelled_) {
            pollfd descriptor{fd_,POLLIN,0};int rc=poll(&descriptor,1,100);
            if(rc<0){if(errno==EINTR)continue;fail(errno);return;}
            if(!rc)continue;
            if(!(descriptor.revents&POLLIN)){fail(ECONNRESET);return;}
            iovec buffer{packet.data(),packet.size()};msghdr message{};message.msg_iov=&buffer;message.msg_iovlen=1;
            ssize_t count=recvmsg(fd_,&message,MSG_DONTWAIT);
            if(count<0){if(errno==EINTR || errno==EAGAIN)continue;fail(errno);return;}
            if(!count){fail(ECONNRESET);return;}
            if(message.msg_flags&(MSG_TRUNC|MSG_CTRUNC) || count<static_cast<ssize_t>(sizeof(ResponseHeader))){fail(EPROTO);return;}
            ResponseHeader h;std::memcpy(&h,packet.data(),sizeof(h));
            if(h.length!=count){fail(EPROTO);return;}
            size_t payload=count-sizeof(h);
            if(!h.unique) {
                if(!invalidation(h,packet.data()+sizeof(h),payload)){fail(EPROTO);return;}
                if(++notifications>=64){fail(ELOOP);return;}continue;
            }
            notifications=0;
            if(h.error>0 || h.error<-4095 || (h.error && payload)){fail(EPROTO);return;}
            bool invalid=false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto entry=pending_.find(h.unique);
                invalid=entry==pending_.end() || entry->second->done;
                if(!invalid) {
                    auto& p=*entry->second;p.error=h.error;
                    if(!h.error)p.bytes.assign(packet.begin()+sizeof(h),packet.begin()+count);
                    p.done=true;p.ready.notify_all();
                }
            }
            if(invalid){fail(EPROTO);return;}
        }
    }
public:
    static constexpr size_t MaxBody=65536,MaxPacket=MaxBody+1024;
    static constexpr size_t MaxPending=128;
    explicit Channel(int ownedFd,int timeoutMs=30000):fd_(ownedFd),timeoutMs_(timeoutMs) {
        int type=0;socklen_t length=sizeof(type);
        if(timeoutMs<=0 || getsockopt(fd_,SOL_SOCKET,SO_TYPE,&type,&length) || type!=SOCK_SEQPACKET)failed_=true;
        else reader_=std::thread([this]{receive();});
    }
    Channel(const Channel&)=delete;
    Channel& operator=(const Channel&)=delete;
    // Callers join their broker workers before destroying the shared channel.
    ~Channel(){cancel();if(reader_.joinable())reader_.join();close(fd_);}
    void cancel(){fail(ECANCELED);}
    int call(uint32_t opcode,uint64_t node,Identity identity,const void* body,size_t size,std::vector<char>* output) {
        if(!output)return -EINVAL;
        output->clear();
        if(size>MaxBody || (size && !body))return -EINVAL;
        auto deadline=Clock::now()+std::chrono::milliseconds(timeoutMs_);
        auto pending=std::make_shared<Pending>();uint64_t id;
        std::unique_lock<std::timed_mutex> sending(sending_,std::defer_lock);
        if(!sending.try_lock_until(deadline))return -ETIMEDOUT;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(failed_ || cancelled_)return -ECANCELED;
            if(unique_==UINT64_MAX)return -EOVERFLOW;
            if(pending_.size()>=MaxPending)return -EAGAIN;
            id=++unique_;pending_[id]=pending;
        }
        send(opcode,node,identity,body,size,id,deadline);sending.unlock();
        std::unique_lock<std::mutex> lock(mutex_);
        if(!pending->ready.wait_until(lock,deadline,[&]{return pending->done;})) {
            lock.unlock();fail(ETIMEDOUT);lock.lock();
        }
        int result=pending->error;*output=std::move(pending->bytes);pending_.erase(id);return result;
    }
    int forget(uint64_t node,uint64_t references,Identity identity) {
        auto deadline=Clock::now()+std::chrono::milliseconds(timeoutMs_);uint64_t id;
        std::unique_lock<std::timed_mutex> sending(sending_,std::defer_lock);
        if(!sending.try_lock_until(deadline))return -ETIMEDOUT;
        {
            std::lock_guard<std::mutex> lock(mutex_);if(failed_ || cancelled_)return -ECANCELED;
            if(unique_==UINT64_MAX)return -EOVERFLOW;
            id=++unique_;
        }
        return send(2,node,identity,&references,sizeof(references),id,deadline);
    }
};
}
