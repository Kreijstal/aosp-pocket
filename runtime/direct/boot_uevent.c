// SPDX-License-Identifier: GPL-3.0-only
// Virtual hardware event provider. No Bionic hooks or host netlink access.
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// Guest device events belong to a private channel, never the host's kernel
// uevent netlink socket. An explicit relay may inject records in the future.
static pthread_mutex_t event_mutex=PTHREAD_MUTEX_INITIALIZER;
static int event_pair[2]={-1,-1};
static pid_t event_pid;
int uevent_init(void) {
    pthread_mutex_lock(&event_mutex);
    if(event_pid!=getpid()) {
        if(event_pair[0]>=0)close(event_pair[0]);
        if(event_pair[1]>=0)close(event_pair[1]);
        event_pair[0]=event_pair[1]=-1;event_pid=getpid();
    }
    int ok=event_pair[0]>=0 || socketpair(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0,event_pair)==0;
    pthread_mutex_unlock(&event_mutex);return ok;
}
int uevent_get_fd(void) {return uevent_init()?event_pair[0]:-1;}
int uevent_next_event(char* buffer,int capacity) {
    if(!buffer || capacity<2) {errno=EINVAL;return -1;}
    int fd=uevent_get_fd();if(fd<0)return -1;
    ssize_t count;
    do {count=recv(fd,buffer,capacity-1,0);}while(count<0 && errno==EINTR);
    if(count>=0)buffer[count]=0;
    return count;
}
int pocket_uevent_inject(const char* buffer,int length) {
    if(!buffer || length<1 || length>4096 || buffer[length-1]) {errno=EINVAL;return -1;}
    if(!uevent_init())return -1;
    return send(event_pair[1],buffer,length,MSG_NOSIGNAL);
}
