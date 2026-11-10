// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

// Same app-owned CE protocol used by the older integration. This caller is
// reached only after the typed vold adapter authenticates its Binder caller.
inline bool ceEnabled32(){return access("/.pocket-ce-enabled",F_OK)==0;}
inline int ceControl32(unsigned operation,const std::vector<uint8_t>& secret={}) {
    struct Request {uint32_t magic,version,op,nr,uid,gid;uint64_t args[6];uint32_t p,q,data,groups;};
    static_assert(sizeof(Request)==88,"CE request ABI");
    Request request{0x31454350,1,3,operation,0,0,{0},0,0,static_cast<uint32_t>(secret.size()),0};
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -errno;
    timeval timeout{30,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/pocket_ce");
    auto transfer=[&](void* value,size_t size,bool writing) {
        char* bytes=static_cast<char*>(value);
        while(size) {ssize_t n=writing?send(fd,bytes,size,MSG_NOSIGNAL):recv(fd,bytes,size,0);if(n<0 && errno==EINTR)continue;if(n<=0)return false;bytes+=n;size-=n;}
        return true;
    };
    int result=-EIO;
    if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)))result=-errno;
    else if(transfer(&request,sizeof(request),true) && (secret.empty() || transfer(const_cast<uint8_t*>(secret.data()),secret.size(),true))) {
        uint8_t tag=0;struct Reply {int64_t value;uint32_t size,fds;} reply{};
        static_assert(sizeof(Reply)==16,"CE reply ABI");
        if(transfer(&tag,1,false) && tag==0xa5 && transfer(&reply,sizeof(reply),false) && !reply.size && !reply.fds
                && reply.value>=-4095 && reply.value<=16384)result=static_cast<int>(reply.value);
    }
    close(fd);return result;
}
class CeSecret32 {
public:
    std::vector<uint8_t> bytes;
    ~CeSecret32(){erase();}
    void erase(){volatile uint8_t* next=bytes.data();for(size_t i=0;i<bytes.size();i++)next[i]=0;bytes.clear();}
    bool decode(const std::string& encoded) {
        erase();if(encoded.empty() || encoded=="!")return true;
        if(encoded.size()%2 || encoded.size()>4096)return false;
        auto digit=[](unsigned char c)->int {if(c>='0' && c<='9')return c-'0';if(c>='a' && c<='f')return c-'a'+10;if(c>='A' && c<='F')return c-'A'+10;return -1;};
        for(size_t i=0;i<encoded.size();i+=2) {
            int a=digit(encoded[i]),b=digit(encoded[i+1]);if(a<0 || b<0){erase();return false;}bytes.push_back((a<<4)|b);
        }
        return true;
    }
};
