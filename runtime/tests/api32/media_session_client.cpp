// SPDX-License-Identifier: GPL-3.0-only
#include "../../api32/fuse_files.h"
#include <sys/un.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"MEDIA_FUSE32_FAIL line=%d errno=%d\n",__LINE__,errno);std::exit(1);}}while(0)
using namespace pocket::fuse32;
#ifdef POCKET_EXTERNAL_NAMESPACE32
#include "media_namespace_proof.h"
#endif
#ifdef POCKET_EXTERNAL_PROXY32
#include "media_proxy_server.h"
#endif
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);REQUIRE(argc==3 && strlen(argv[1])==32);
    const std::string package="dev.kreijstal.pocketmediasession32";REQUIRE(argv[2]==package);
    REQUIRE(getuid()>=10000);int control=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);REQUIRE(control>=0);
    sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/pms32");
    REQUIRE(connect(control,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    REQUIRE(write(control,argv[1],32)==32);
    char tag,ancillary[CMSG_SPACE(sizeof(int))]{};iovec part{&tag,1};msghdr message{};
    message.msg_iov=&part;message.msg_iovlen=1;message.msg_control=ancillary;message.msg_controllen=sizeof(ancillary);
    REQUIRE(recvmsg(control,&message,MSG_CMSG_CLOEXEC)==1 && tag=='F' && !(message.msg_flags&MSG_CTRUNC));
    cmsghdr* rights=CMSG_FIRSTHDR(&message);REQUIRE(rights && rights->cmsg_level==SOL_SOCKET && rights->cmsg_type==SCM_RIGHTS && rights->cmsg_len==CMSG_LEN(sizeof(int)));
    int fd;memcpy(&fd,CMSG_DATA(rights),sizeof(fd));
    {
        Channel channel(fd,12000);Files files(channel);Identity identity{getuid(),getgid(),static_cast<uint32_t>(syscall(__NR_gettid))};std::vector<char> out;
        int result=files.initialize(identity);printf("INIT_RESULT=%d\n",result);
        REQUIRE(result==0);printf("MEDIA_FUSE32_INIT_PASS protocol=7.31 uid=%u\n",identity.uid);
        REQUIRE(write(control,"I",1)==1 && read(control,&tag,1)==1 && tag=='G');
#ifdef POCKET_EXTERNAL_NAMESPACE32
        REQUIRE(write(control,"N",1)==1);
        mediaNamespaceProof(files,identity,package,argv[1]);
        REQUIRE(write(control,"S",1)==1);
#endif
        // Stock12L deliberately hides the non-user /storage/emulated root
        // from app getattr. LOOKUP of its user child is explicitly allowed.
        fuse_getattr_in attr{};int root=channel.call(FUSE_GETATTR,1,identity,&attr,sizeof(attr),&out);
        printf("ROOT_GETATTR_RESULT=%d\n",root);REQUIRE(root==-ENOENT);
        std::vector<uint64_t> nodes;uint64_t parent=1;
        auto lookup=[&](const std::string& name){int code=channel.call(FUSE_LOOKUP,parent,identity,name.c_str(),name.size()+1,&out);
            printf("LOOKUP component=%s result=%d\n",name.c_str(),code);return code;};
        for(const auto& name:std::vector<std::string>{"0","Android","data",package}) {
            REQUIRE(lookup(name)==0 && out.size()>=sizeof(fuse_entry_out));fuse_entry_out entry{};memcpy(&entry,out.data(),sizeof(entry));
            REQUIRE(S_ISDIR(entry.attr.mode));
            REQUIRE(entry.nodeid>1);parent=entry.nodeid;nodes.push_back(parent);
        }
        // Even the caller's own emulated Android/data directory bypasses FUSE
        // on12L. The future path broker must authorize a lower view for it;
        // changing MediaProvider to permit these opens would break its model.
        REQUIRE(channel.call(FUSE_GETATTR,parent,identity,&attr,sizeof(attr),&out)==-ENOENT);
        parent=nodes[2];int denied=lookup(package+".other");
        if(!denied) {
            REQUIRE(out.size()>=sizeof(fuse_entry_out));fuse_entry_out other{};memcpy(&other,out.data(),sizeof(other));nodes.push_back(other.nodeid);
            denied=channel.call(FUSE_GETATTR,other.nodeid,identity,&attr,sizeof(attr),&out);
        }
        REQUIRE(denied==-EACCES || denied==-ENOENT || denied==-EPERM);
        for(auto node:nodes)REQUIRE(channel.forget(node,1,identity)==0);
#ifdef POCKET_EXTERNAL_PROXY32
        mediaProxy(control,files,argv[1],package);
#endif
        REQUIRE(channel.call(FUSE_DESTROY,0,identity,nullptr,0,&out)==0);channel.cancel();
    }
    REQUIRE(write(control,"D",1)==1);close(control);
    puts("MEDIA_FUSE32_PASS provider=real user_lookup=verified app_root=denied app_private_fuse=denied cross_package=denied kernel_mounts=0");return 0;
}
