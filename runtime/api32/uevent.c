// SPDX-License-Identifier: GPL-3.0-only
// Device events for a userspace guest with no kernel device driver namespace.
#include <hardware_legacy/uevent.h>
#include "uevent.h"
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static int channel[2]={-1,-1};
static void prepare_fork(void) {pthread_mutex_lock(&mutex);}
static void parent_fork(void) {pthread_mutex_unlock(&mutex);}
static void child_fork(void) {
    for(int i=0;i<2;i++)if(channel[i]>=0)close(channel[i]);
    channel[0]=channel[1]=-1;
    pthread_mutex_unlock(&mutex);
}
__attribute__((constructor)) static void install_fork_handlers(void) {
    // A child must not consume or inject events belonging to its parent.
    if(pthread_atfork(prepare_fork,parent_fork,child_fork))_exit(127);
}
int uevent_init(void) {
    pthread_mutex_lock(&mutex);
    int ready=channel[0]>=0 || socketpair(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0,channel)==0;
    pthread_mutex_unlock(&mutex);
    return ready;
}
int uevent_get_fd(void) {return uevent_init()?channel[0]:-1;}
int uevent_next_event(char* buffer,int capacity) {
    if(!buffer || capacity<2){errno=EINVAL;return -1;}
    int fd=uevent_get_fd();if(fd<0)return -1;
    struct iovec part={buffer,(size_t)capacity-1};
    struct msghdr message={0};message.msg_iov=&part;message.msg_iovlen=1;
    ssize_t count;
    do {count=recvmsg(fd,&message,0);}while(count<0 && errno==EINTR);
    if(count<0)return -1;
    if((message.msg_flags&MSG_TRUNC) || count==0 || buffer[count-1]!=0) {
        buffer[0]=0;errno=EMSGSIZE;return -1;
    }
    buffer[count]=0;
    return (int)count;
}
int pocket_uevent_inject32(const char* record,int length) {
    if(!record || length<1 || length>POCKET_UEVENT_MAX32 || record[length-1]!=0) {
        errno=EINVAL;return -1;
    }
    if(!uevent_init())return -1;
    // Event delivery must never stall a device producer when a consumer stops.
    return (int)send(channel[1],record,(size_t)length,MSG_NOSIGNAL|MSG_DONTWAIT);
}
