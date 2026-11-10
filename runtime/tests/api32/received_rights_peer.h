// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../../api32/received_rights.h"
#include <cassert>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
static unsigned rightsDescriptorCount32() {
    DIR* directory=opendir("/proc/self/fd");assert(directory);unsigned count=0;
    while(readdir(directory))count++;
    closedir(directory);return count;
}
static void receivedRightsErrors32() {
    const unsigned before=rightsDescriptorCount32();
    int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair));
    timeval timeout{0,100000};assert(!setsockopt(pair[0],SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
    char byte=0,control[CMSG_SPACE(2*sizeof(int))+1]{};std::vector<int> rights;
    auto receive=[&] {
        iovec part{&byte,1};msghdr message{};message.msg_iov=&part;message.msg_iovlen=1;
        message.msg_control=control+1;message.msg_controllen=sizeof(control)-1;
        ssize_t n=recvmsg(pair[0],&message,MSG_CMSG_CLOEXEC);int error=n<0?errno:0;
        return pocket::decodeRights32(message,n,error,sizeof(control)-1,&rights);
    };
    assert(receive()==-EAGAIN && rights.empty());
    int original=open("/dev/null",O_RDONLY|O_CLOEXEC);assert(original>=0);
    for(unsigned count:{1U,9U}) {
        char ancillary[CMSG_SPACE(9*sizeof(int))]{};iovec part{&byte,1};msghdr message{};
        message.msg_iov=&part;message.msg_iovlen=1;message.msg_control=ancillary;message.msg_controllen=CMSG_SPACE(count*sizeof(int));
        cmsghdr header{};header.cmsg_level=SOL_SOCKET;header.cmsg_type=SCM_RIGHTS;header.cmsg_len=CMSG_LEN(count*sizeof(int));
        memcpy(ancillary,&header,sizeof(header));
        for(unsigned i=0;i<count;i++)memcpy(ancillary+CMSG_LEN(0)+i*sizeof(int),&original,sizeof(original));
        assert(sendmsg(pair[1],&message,MSG_NOSIGNAL)==1);
        int decoded=receive();assert(decoded==(count==1?0:-EPROTO));
        assert(count==1?rights.size()==1:!rights.empty());
        for(int fd:rights){assert(fcntl(fd,F_GETFD)&FD_CLOEXEC);assert(!close(fd));}rights.clear();
    }
    assert(!close(original) && !close(pair[1]));assert(receive()==-ECONNRESET && rights.empty());assert(!close(pair[0]));
    // Exercise malformed and unaligned input independently of kernel header
    // sanitization. No valid FD values are present in these synthetic frames.
    msghdr malformed{};malformed.msg_control=control+1;malformed.msg_controllen=sizeof(control)-1;memset(control,0,sizeof(control));
    assert(pocket::decodeRights32(malformed,1,0,sizeof(control)-1,&rights)==-EPROTO);
    cmsghdr header{};header.cmsg_len=sizeof(control)+1;memcpy(control+1,&header,sizeof(header));
    assert(pocket::decodeRights32(malformed,1,0,sizeof(control)-1,&rights)==-EPROTO);
    malformed.msg_controllen=1;assert(pocket::decodeRights32(malformed,1,0,sizeof(control)-1,&rights)==-EPROTO);
    malformed.msg_controllen=sizeof(control)+1;assert(pocket::decodeRights32(malformed,1,0,sizeof(control)-1,&rights)==-EPROTO);
    malformed.msg_control=nullptr;malformed.msg_controllen=sizeof(cmsghdr);assert(pocket::decodeRights32(malformed,1,0,sizeof(control)-1,&rights)==-EPROTO);
    assert(rights.empty() && rightsDescriptorCount32()==before);
    puts("RECEIVED_RIGHTS32_PASS timeout=1 eof=1 actual_rights=1 truncated_rights_closed=1 malformed_length=1 unaligned_control=1 fd_leaks=0");
}
