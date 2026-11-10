// SPDX-License-Identifier: GPL-3.0-only
#include <hardware_legacy/uevent.h>
#include "uevent.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) do {if(!(condition)){fprintf(stderr,"API32_UEVENT_FAILURE line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
int main(void) {
    CHECK(uevent_init());int fd=uevent_get_fd();CHECK(fd>=0 && fd==uevent_get_fd());
    struct sockaddr_storage address;socklen_t size=sizeof(address);
    CHECK(!getsockname(fd,(struct sockaddr*)&address,&size) && address.ss_family==AF_UNIX);
    int type=0;size=sizeof(type);CHECK(!getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size) && type==SOCK_DGRAM);
    CHECK(fcntl(fd,F_GETFD)&FD_CLOEXEC);
    const char record[]="change@/devices/pocket-api32-test\0ACTION=change\0DEVPATH=/devices/pocket-api32-test\0";
    CHECK(pocket_uevent_inject32(record,sizeof(record))==sizeof(record));
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(uevent_init());
        struct pollfd waiting={uevent_get_fd(),POLLIN,0};CHECK(poll(&waiting,1,0)==0);
        CHECK(pocket_uevent_inject32(record,sizeof(record))==sizeof(record));
        char copy[256];CHECK(uevent_next_event(copy,sizeof(copy))==sizeof(record));
        CHECK(!memcmp(copy,record,sizeof(record)));_exit(0);
    }
    int status=0;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    char copy[256];CHECK(uevent_next_event(copy,sizeof(copy))==sizeof(record));
    CHECK(!memcmp(copy,record,sizeof(record)));
    CHECK(pocket_uevent_inject32(record,2)==-1 && errno==EINVAL);
    char large[POCKET_UEVENT_MAX32+1]={0};CHECK(pocket_uevent_inject32(large,sizeof(large))==-1 && errno==EINVAL);
    CHECK(pocket_uevent_inject32(record,sizeof(record))==sizeof(record));
    CHECK(uevent_next_event(copy,8)==-1 && errno==EMSGSIZE);
    struct pollfd waiting={fd,POLLIN,0};CHECK(poll(&waiting,1,0)==0);
    CHECK(uevent_next_event(NULL,2)==-1 && errno==EINVAL);
    puts("API32_UEVENT_CHANNEL_PASS private_datagram=1 cloexec=1 fork_isolation=1 invalid_rejected=1 truncation_rejected=1 host_netlink=unused source=synthetic_test");
    return 0;
}
