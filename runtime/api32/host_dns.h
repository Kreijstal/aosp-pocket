// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// HostWire v1 DNS and DNS_SERVERS payloads, shared by resolver/probe code.
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <string>
#include <vector>
#include "dns_packet.h"

inline bool dnsTransfer32(int fd,void* bytes,size_t size,bool writing) {
    char* next=static_cast<char*>(bytes);
    while(size) {
        ssize_t count=writing?send(fd,next,size,MSG_NOSIGNAL):recv(fd,next,size,0);
        if(count<0 && errno==EINTR)continue;
        if(count<=0)return false;
        next+=count;size-=count;
    }
    return true;
}
inline bool dnsWord32(int fd,uint32_t value) {value=htonl(value);return dnsTransfer32(fd,&value,4,true);}
inline bool dnsBlock32(int fd,const void* value,size_t size) {
    return dnsWord32(fd,size) && dnsTransfer32(fd,const_cast<void*>(value),size,true);
}
inline bool hostDns32(unsigned operation,const std::string& request,std::vector<std::vector<uint8_t>>& addresses) {
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return false;
    timeval timeout{12,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/pocket_host");
    uint32_t status=0,size=0;
    bool ok=!connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)) && dnsWord32(fd,0x504b4831) &&
        dnsWord32(fd,operation) && dnsBlock32(fd,request.data(),request.size()) &&
        dnsTransfer32(fd,&status,4,false) && dnsTransfer32(fd,&size,4,false);
    size=ntohl(size);status=ntohl(status);std::vector<uint8_t> payload;
    if(ok && !status && size>=4 && size<=4096){payload.resize(size);ok=dnsTransfer32(fd,payload.data(),size,false);}else ok=false;
    close(fd);if(!ok)return false;
    size_t offset=0;
    auto word=[&](uint32_t& value){
        if(offset+4>payload.size())return false;
        memcpy(&value,payload.data()+offset,4);offset+=4;value=ntohl(value);return true;
    };
    uint32_t count=0;if(!word(count) || count>64)return false;
    addresses.clear();
    for(unsigned n=0;n<count;++n) {
        uint32_t length=0;
        if(!word(length) || (length!=4 && length!=16) || offset+length>payload.size())return false;
        addresses.emplace_back(payload.begin()+offset,payload.begin()+offset+length);offset+=length;
    }
    return offset==payload.size();
}
inline std::vector<std::string> dnsStrings32(const std::vector<std::vector<uint8_t>>& addresses) {
    std::vector<std::string> result;
    for(const auto& address:addresses) {
        char text[INET6_ADDRSTRLEN];
        if(inet_ntop(address.size()==4?AF_INET:AF_INET6,address.data(),text,sizeof(text)))result.emplace_back(text);
    }
    return result;
}
inline int hostDnsPacket32(unsigned flags,bool bypass,const std::vector<uint8_t>& query,
        unsigned& rcode,std::vector<uint8_t>& answer) {
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -errno;
    timeval timeout{12,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/pocket_host");
    uint32_t status=0,size=0,code=0;int result=-EIO;
    bool ok=!connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)) && dnsWord32(fd,0x504b4831)
        && dnsWord32(fd,17) && dnsWord32(fd,8+query.size()) && dnsWord32(fd,flags) && dnsWord32(fd,bypass?1:0)
        && dnsTransfer32(fd,const_cast<uint8_t*>(query.data()),query.size(),true)
        && dnsTransfer32(fd,&status,4,false) && dnsTransfer32(fd,&size,4,false);
    status=ntohl(status);size=ntohl(size);
    if(ok && status)result=status==403?-EACCES:status==400?-EINVAL:status==404?-EOPNOTSUPP:status==504?-ETIMEDOUT:-EIO;
    else if(ok && size>=16 && size<=8196 && dnsTransfer32(fd,&code,4,false)) {
        rcode=ntohl(code);answer.resize(size-4);
        if(rcode<=4095 && dnsTransfer32(fd,answer.data(),answer.size(),false))result=0;
    }
    close(fd);if(result)answer.clear();return result;
}
inline bool hostDnsBypass32(const std::string& hostname,int family,bool v4mapped,
        std::vector<std::vector<uint8_t>>& addresses) {
    addresses.clear();
    for(unsigned type:{1U,28U}) {
        if((type==1 && family==AF_INET6 && !v4mapped) || (type==28 && family==AF_INET))continue;
        std::vector<uint8_t> query,answer;unsigned rcode=0;
        if(!dnsQuery32(hostname,type,query))return false;
        if(hostDnsPacket32(0,true,query,rcode,answer) || rcode)continue;
        auto found=dnsAddresses32(answer,type);addresses.insert(addresses.end(),found.begin(),found.end());
    }
    return !addresses.empty();
}
