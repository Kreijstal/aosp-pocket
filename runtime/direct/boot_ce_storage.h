// SPDX-License-Identifier: GPL-3.0-only
#include <sys/socket.h>
#include <sys/un.h>
#include <stdint.h>
#include <vector>
// Same-UID private RPC. The existing vold Binder caller checks remain intact.
static int ceControl(unsigned operation,const std::vector<uint8_t>& secret={}) {
    struct Request {uint32_t magic,version,op,nr,uid,gid;uint64_t args[6];uint32_t p,q,data,groups;};
    static_assert(sizeof(Request)==88,"CE wire ABI");
    Request request={0x31454350,1,3,operation,0,0,{0},0,0,(uint32_t)secret.size(),0};
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -errno;
    sockaddr_un address={};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/pocket_ce");
    auto transfer=[&](void* data,size_t size,bool writing)->bool {
        char* bytes=(char*)data;while(size){ssize_t n=writing?write(fd,bytes,size):read(fd,bytes,size);if(n<0&&errno==EINTR)continue;if(n<=0)return false;bytes+=n;size-=n;}return true;
    };
    int result=-EIO;
    if(!connect(fd,(sockaddr*)&address,sizeof(address)) && transfer(&request,sizeof(request),true) &&
       (secret.empty() || transfer((void*)secret.data(),secret.size(),true))) {
        uint8_t tag;struct Reply {int64_t value;uint32_t size,fds;} reply;
        if(transfer(&tag,1,false)&&tag==0xa5&&transfer(&reply,sizeof(reply),false)&&!reply.size&&!reply.fds)result=(int)reply.value;
    }
    close(fd);return result;
}
static bool ceSecret(const android::String16& encoded,std::vector<uint8_t>* out) {
    if(!encoded.size() || encoded==android::String16("!"))return true;
    if(encoded.size()%2 || encoded.size()>4096)return false;
    auto nibble=[](char16_t c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
    for(size_t i=0;i<encoded.size();i+=2){int a=nibble(encoded[i]),b=nibble(encoded[i+1]);if(a<0||b<0)return false;out->push_back((a<<4)|b);}return true;
}
static bool ceEnabled() {return access("/.pocket-ce-enabled",F_OK)==0;}
