// SPDX-License-Identifier: GPL-3.0-only
// Translate authenticated profile identities on traced's two private endpoints.
// Kernel credentials and the host sandbox remain unchanged.
#include "../api28/binder_identity.h"
#include "../direct/kernel_identity.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

namespace {
struct Peer { bool used; int fd; ucred kernel; uid_t guest; };
Peer peers[512];
pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
bool endpoint(int fd) {
    sockaddr_un address={};socklen_t length=sizeof(address);
    if(getsockname(fd,reinterpret_cast<sockaddr*>(&address),&length) || address.sun_family!=AF_UNIX)return false;
    address.sun_path[sizeof(address.sun_path)-1]=0;
    const char* tails[]={"/dev/socket/traced_consumer","/dev/socket/traced_producer"};
    for(const char* tail:tails) {
        size_t n=strlen(address.sun_path),m=strlen(tail);
        if(n>=m && !strcmp(address.sun_path+n-m,tail))return true;
    }
    return false;
}
int acceptPeer(int listener,sockaddr* address,socklen_t* length,int flags) {
    bool scoped=endpoint(listener);
    // Bound each dispatch even if an unauthenticated client repeatedly connects.
    for(unsigned attempt=0;attempt<32;++attempt) {
        int fd=syscall(__NR_accept4,listener,address,length,flags);
        if(fd<0 || !scoped)return fd;
        static const uid_t host=pocket_kernel_uid_from_proc();
        ucred peer={};socklen_t size=sizeof(peer);uint64_t identity=0;
        bool valid=host!=static_cast<uid_t>(-1) &&
            !syscall(__NR_getsockopt,fd,SOL_SOCKET,SO_PEERCRED,&peer,&size) && size==sizeof(peer) &&
            peer.uid==host && pocket_binder_identity(peer.pid,&identity);
        bool saved=false;
        if(valid) {
            pthread_mutex_lock(&mutex);
            for(auto& row:peers)if(!row.used) {
                row={true,fd,peer,static_cast<uid_t>(identity>>32)};saved=true;break;
            }
            pthread_mutex_unlock(&mutex);
        }
        if(saved)return fd;
        syscall(__NR_close,fd);
    }
    errno=EAGAIN;return -1;
}
}
extern "C" int accept(int fd,sockaddr* address,socklen_t* length) {
    return acceptPeer(fd,address,length,0);
}
extern "C" int accept4(int fd,sockaddr* address,socklen_t* length,int flags) {
    return acceptPeer(fd,address,length,flags);
}
extern "C" int getsockopt(int fd,int level,int option,void* value,socklen_t* size) {
    int result=syscall(__NR_getsockopt,fd,level,option,value,size);
    if(result || level!=SOL_SOCKET || option!=SO_PEERCRED || !value || !size || *size!=sizeof(ucred))return result;
    auto* peer=static_cast<ucred*>(value);
    pthread_mutex_lock(&mutex);
    for(const auto& row:peers)if(row.used && row.fd==fd && row.kernel.pid==peer->pid &&
            row.kernel.uid==peer->uid && row.kernel.gid==peer->gid) {
        peer->uid=row.guest;break;
    }
    pthread_mutex_unlock(&mutex);
    return result;
}
extern "C" int close(int fd) {
    pthread_mutex_lock(&mutex);
    for(auto& row:peers)if(row.used && row.fd==fd)row={};
    // Hold the lock through close so reuse of this fd cannot erase a new row.
    int result=syscall(__NR_close,fd);
    pthread_mutex_unlock(&mutex);
    return result;
}
