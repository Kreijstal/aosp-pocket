// SPDX-License-Identifier: GPL-3.0-only
#include "protocol.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int countFds(pid_t pid) {
    char path[80];snprintf(path,sizeof(path),"/proc/%d/fd",pid);
    DIR* directory=opendir(path);if(!directory)return -1;
    int count=0;while(auto* entry=readdir(directory))if(entry->d_name[0]!='.')++count;
    closedir(directory);return count;
}
static bool rejected(pid_t target) {
    int sock=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);if(sock<0)return false;
    sockaddr_un address={};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/tombstoned_intercept");
    timeval timeout={3,0};setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    bool connected=false;
    for(unsigned i=0;i<100;++i) {
        if(!connect(sock,reinterpret_cast<sockaddr*>(&address),sizeof(address))){connected=true;break;}
        if(errno!=ECONNREFUSED && errno!=ENOENT)break;
        usleep(20000);
    }
    if(!connected){close(sock);return false;}
    int output=open("/dev/null",O_WRONLY|O_CLOEXEC);if(output<0){close(sock);return false;}
    InterceptRequest request={kDebuggerdJavaBacktrace,target};iovec iov{&request,sizeof(request)};
    alignas(cmsghdr) unsigned char control[CMSG_SPACE(sizeof(int))]={};
    msghdr message={};message.msg_iov=&iov;message.msg_iovlen=1;message.msg_control=control;message.msg_controllen=sizeof(control);
    cmsghdr* c=CMSG_FIRSTHDR(&message);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c),&output,sizeof(output));bool sent=sendmsg(sock,&message,MSG_NOSIGNAL)==sizeof(request);close(output);
    InterceptResponse response={};ssize_t got=sent?recv(sock,&response,sizeof(response),0):-1;
    bool denied=sent && (got==0 || (got==sizeof(response) && response.status==InterceptStatus::kFailed));
    close(sock);return denied;
}
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);if(argc!=3 || getuid()!=0)return 64;
    pid_t server=atoi(argv[2]);if(server<=1 || server==getpid())return 64;
    bool outside=!strcmp(argv[1],"outside");
    pid_t target=outside?atoi(getenv("POCKET_API32_HOST_PID")?getenv("POCKET_API32_HOST_PID"):"0"):getpid();
    if(outside) {if(target<=1 || target==server || target==getpid())return 2;}
    else if(strcmp(argv[1],"denied") || setgid(39999) || setuid(39999))return 64;
    if(!rejected(target))return 5; // Also waits for the actual daemon event loop.
    int before=countFds(server);if(before<0)return 1;
    for(unsigned i=0;i<24;++i)if(!rejected(target))return 3;
    int after=-1;for(unsigned i=0;i<100;++i){after=countFds(server);if(after==before)break;usleep(20000);}
    if(after!=before)return 4;
    printf("API32_TOMBSTONED_%s_PASS uid=%u requests=24 transferred_fds=closed daemon_fd_before=%d daemon_fd_after=%d\n",
        outside?"OUTSIDE_PROFILE_DENIED":"ROLE_DENIED",getuid(),before,after);
    return 0;
}
