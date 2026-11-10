// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <sys/socket.h>
#include <cerrno>
#include <cstring>
#include <limits>
#include <vector>

namespace pocket {
// Decode SCM_RIGHTS only after a successful Unix-stream receive. On error,
// msg_controllen still describes the caller's input buffer; it contains no
// received headers. In particular, Bionic CMSG_NXTHDR does not advance past
// a zero-length header. The caller owns every appended FD even on EPROTO,
// including the descriptors the kernel delivered with MSG_CTRUNC.
inline int decodeRights32(const msghdr& message,ssize_t count,int receiveError,
                          size_t capacity,std::vector<int>* descriptors) {
    if(count<=0)return count<0?-(receiveError>0 && receiveError<=4095?receiveError:EIO):-ECONNRESET;
    if(!descriptors || message.msg_controllen>capacity || (message.msg_controllen && !message.msg_control))return -EPROTO;
    if(message.msg_controllen && message.msg_controllen<sizeof(cmsghdr))return -EPROTO;
    bool valid=!(message.msg_flags&(MSG_TRUNC|MSG_CTRUNC));
    const auto* bytes=static_cast<const char*>(message.msg_control);
    for(size_t at=0;message.msg_controllen-at>=sizeof(cmsghdr);) {
        // memcpy also handles control buffers that are not cmsghdr-aligned.
        cmsghdr header{};memcpy(&header,bytes+at,sizeof(header));
        const size_t left=message.msg_controllen-at;
        if(header.cmsg_len<CMSG_LEN(0) || header.cmsg_len>left){valid=false;break;}
        if(header.cmsg_level!=SOL_SOCKET || header.cmsg_type!=SCM_RIGHTS)valid=false;
        else {
            const size_t payload=header.cmsg_len-CMSG_LEN(0);
            if(!payload || payload%sizeof(int))valid=false;
            for(size_t i=0;i+sizeof(int)<=payload;i+=sizeof(int)) {
                int fd;memcpy(&fd,bytes+at+CMSG_LEN(0)+i,sizeof(fd));descriptors->push_back(fd);
            }
        }
        // The last record need not include its alignment padding. Checking
        // the addition before CMSG_ALIGN also avoids size_t wraparound.
        if(header.cmsg_len>std::numeric_limits<size_t>::max()-(sizeof(size_t)-1)){valid=false;break;}
        const size_t step=CMSG_ALIGN(header.cmsg_len);if(step>left)break;
        at+=step;
    }
    return valid?0:-EPROTO;
}
}
