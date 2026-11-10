// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <limits>

inline bool dnsUnsigned32(const std::string& text,unsigned& result) {
    if(text.empty())return false;result=0;
    for(unsigned char c:text) {
        if(c<'0' || c>'9' || result>(std::numeric_limits<unsigned>::max()-(c-'0'))/10)return false;
        result=result*10+(c-'0');
    }
    return true;
}

inline bool dnsDecode6432(const std::string& text,std::vector<uint8_t>& bytes) {
    bytes.clear();if(text.empty() || text.size()%4 || text.size()>10924)return false;
    auto digit=[](char c)->int {
        if(c>='A' && c<='Z')return c-'A';if(c>='a' && c<='z')return c-'a'+26;
        if(c>='0' && c<='9')return c-'0'+52;if(c=='+')return 62;if(c=='/')return 63;return -1;
    };
    for(size_t i=0;i<text.size();i+=4) {
        int a=digit(text[i]),b=digit(text[i+1]);bool cpad=text[i+2]=='=',dpad=text[i+3]=='=';
        int c=cpad?0:digit(text[i+2]),d=dpad?0:digit(text[i+3]);
        if(a<0 || b<0 || c<0 || d<0 || (cpad&&!dpad) || ((cpad||dpad)&&i+4!=text.size())
                || (cpad&&(b&15)) || (dpad&&!cpad&&(c&3))) {bytes.clear();return false;}
        bytes.push_back((a<<2)|(b>>4));
        if(!cpad)bytes.push_back((b<<4)|(c>>2));if(!dpad)bytes.push_back((c<<6)|d);
    }
    if(bytes.size()>8192){bytes.clear();return false;}return true;
}
inline unsigned dnsWordAt32(const std::vector<uint8_t>& bytes,size_t offset) {
    return (static_cast<unsigned>(bytes[offset])<<8)|bytes[offset+1];
}
inline bool dnsQuery32(const std::string& name,unsigned type,std::vector<uint8_t>& packet) {
    packet={0,0,1,0,0,1,0,0,0,0,0,0};
    if(name.empty() || name.size()>254 || !type || type>65535)return false;
    size_t offset=0;
    while(offset<name.size()) {
        size_t end=name.find('.',offset);if(end==std::string::npos)end=name.size();
        size_t size=end-offset;
        if(size==0 || size>63 || packet.size()+size+1>266)return false;
        packet.push_back(size);packet.insert(packet.end(),name.begin()+offset,name.begin()+end);offset=end+1;
    }
    packet.insert(packet.end(),{0,static_cast<uint8_t>(type>>8),static_cast<uint8_t>(type),0,1});return true;
}
inline bool dnsQuestion32(const std::vector<uint8_t>& packet,std::string& name,unsigned* type=nullptr) {
    name.clear();if(packet.size()<17 || packet.size()>8192 || (packet[2]&0xf8) || (packet[3]&15)
        || dnsWordAt32(packet,4)!=1 || dnsWordAt32(packet,6) || dnsWordAt32(packet,8))return false;
    size_t offset=12,length=0;
    for(;;) {
        if(offset>=packet.size())return false;unsigned size=packet[offset++];if(!size)break;
        if(size>63 || offset+size>packet.size() || (length+=size+1)>254)return false;
        if(!name.empty())name+='.';
        name.append(reinterpret_cast<const char*>(packet.data()+offset),size);offset+=size;
    }
    if(offset+4>packet.size() || !dnsWordAt32(packet,offset) || !dnsWordAt32(packet,offset+2))return false;
    if(type)*type=dnsWordAt32(packet,offset);return true;
}
inline bool dnsSkipName32(const std::vector<uint8_t>& packet,size_t& offset) {
    unsigned length=0;
    while(offset<packet.size()) {
        unsigned label=packet[offset++];if(!label)return true;
        if((label&0xc0)==0xc0) {
            if(offset>=packet.size())return false;
            unsigned target=((label&63)<<8)|packet[offset++];return target>=12 && target<offset-2;
        }
        if(label>63 || offset+label>packet.size() || (length+=label+1)>254)return false;
        offset+=label;
    }
    return false;
}
inline std::vector<std::vector<uint8_t>> dnsAddresses32(const std::vector<uint8_t>& packet,unsigned type) {
    std::vector<std::vector<uint8_t>> result;
    if(packet.size()<12 || dnsWordAt32(packet,4)!=1 || (type!=1 && type!=28))return result;
    size_t offset=12;if(!dnsSkipName32(packet,offset) || offset+4>packet.size())return {};offset+=4;
    unsigned answers=dnsWordAt32(packet,6);
    for(unsigned i=0;i<answers;i++) {
        if(!dnsSkipName32(packet,offset) || offset+10>packet.size())return {};
        unsigned rrtype=dnsWordAt32(packet,offset),rrclass=dnsWordAt32(packet,offset+2),size=dnsWordAt32(packet,offset+8);offset+=10;
        if(offset+size>packet.size())return {};
        if(rrtype==type && rrclass==1 && size==(type==1?4U:16U) && result.size()<64)
            result.emplace_back(packet.begin()+offset,packet.begin()+offset+size);
        offset+=size;
    }
    return result;
}
