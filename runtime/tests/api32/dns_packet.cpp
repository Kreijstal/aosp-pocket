// SPDX-License-Identifier: GPL-3.0-only
#include "../../api32/dns_packet.h"
#include <assert.h>
#include <stdio.h>

int main() {
    std::vector<uint8_t> bytes;unsigned number=0;
    assert(dnsUnsigned32("4294967295",number) && number==4294967295U);
    for(const char* bad:{"","-1","+1","4294967296","1x"})assert(!dnsUnsigned32(bad,number));
    for(const auto& item:std::vector<std::pair<std::string,std::string>>{{"Zg==","f"},{"Zm8=","fo"},{"Zm9v","foo"}}) {
        assert(dnsDecode6432(item.first,bytes));assert(std::string(bytes.begin(),bytes.end())==item.second);
    }
    for(const char* bad:{"","A","====","Zg=+","Zh==","Zm9=","Zg==AAAA","Zm$v","Zg==\n"})assert(!dnsDecode6432(bad,bytes));
    assert(!dnsDecode6432(std::string(10924,'A'),bytes) && bytes.empty());
    assert(dnsDecode6432("SjkBAAABAAAAAAAAB2V4YW1wbGUDY29tAAABAAE=",bytes));
    std::string name;unsigned type=0;
    assert(dnsQuestion32(bytes,name,&type) && name=="example.com" && type==1);
    std::vector<uint8_t> constructed;
    assert(dnsQuery32("example.com",1,constructed));constructed[0]=bytes[0];constructed[1]=bytes[1];assert(constructed==bytes);
    assert(dnsQuery32("example.com.",28,constructed) && dnsQuestion32(constructed,name,&type) && name=="example.com" && type==28);
    for(const char* invalid:{"",".example.com","example..com"})assert(!dnsQuery32(invalid,1,constructed));
    assert(!dnsQuery32(std::string(64,'a')+".com",1,constructed));
    for(size_t n=0;n<bytes.size();n++)assert(!dnsQuestion32(std::vector<uint8_t>(bytes.begin(),bytes.begin()+n),name));
    auto bad=bytes;bad[12]=0xc0;assert(!dnsQuestion32(bad,name));
    bad=bytes;bad[12]=64;assert(!dnsQuestion32(bad,name));
    bad=bytes;bad[5]=2;assert(!dnsQuestion32(bad,name));
    bad=bytes;bad[3]=3;assert(!dnsQuestion32(bad,name));
    auto answer=bytes;answer[2]=0x81;answer[3]=0x80;answer[7]=1;
    for(unsigned char byte:{0xc0,0x0c,0,1,0,1,0,0,0,60,0,4,93,184,216,34})answer.push_back(byte);
    auto addresses=dnsAddresses32(answer,1);assert(addresses==std::vector<std::vector<uint8_t>>({{93,184,216,34}}));
    for(size_t n=0;n<answer.size();n++)assert(dnsAddresses32(std::vector<uint8_t>(answer.begin(),answer.begin()+n),1).empty());
    assert(dnsAddresses32(answer,28).empty());
    bad=answer;bad[bytes.size()+11]=28;assert(dnsAddresses32(bad,1).empty());
    bad=answer;bad[bytes.size()]=0xc0;bad[bytes.size()+1]=255;assert(dnsAddresses32(bad,1).empty());
    puts("API32_DNS_PACKET_FIXTURE_PASS integer_overflow=denied base64=canonical truncated_queries_answers=rejected address_records=bounded");
}
