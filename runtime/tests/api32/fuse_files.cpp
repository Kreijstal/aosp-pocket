// SPDX-License-Identifier: GPL-3.0-only
#include "../../api32/fuse_files.h"
#include <cassert>
#include <cstdio>
using namespace pocket::fuse32;

int main() {
    int pair[2];assert(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair)==0);
    Identity opener{10001,10002,20001},actor{10003,10004,20002};
    unsigned flushes=0,syncs=0,releases=0,forgets=0,lookups=0,reads=0,writes=0;
    Channel channel(pair[0],1000);
    std::thread server([&] {
        std::vector<char> packet(Channel::MaxPacket);
        for(;;) {
            ssize_t count=recv(pair[1],packet.data(),packet.size(),0);if(!count)break;
            assert(count>=static_cast<ssize_t>(sizeof(RequestHeader)));
            RequestHeader request;memcpy(&request,packet.data(),sizeof(request));assert(request.length==count);
            const char* body=packet.data()+sizeof(request);size_t size=count-sizeof(request);
            std::vector<char> out;int error=0;
            auto respond=[&](const auto& value){out.resize(sizeof(value));memcpy(out.data(),&value,sizeof(value));};
            bool data=request.opcode==FUSE_READ || request.opcode==FUSE_WRITE || request.opcode==FUSE_FSYNC || request.opcode==FUSE_FLUSH || request.opcode==FUSE_STATFS;
            Identity expected=data?actor:opener;
            assert(request.uid==expected.uid && request.gid==expected.gid && request.pid==expected.pid);
            switch(request.opcode) {
            case FUSE_INIT:{assert(size==16);fuse_init_out value{};value.major=7;value.minor=31;value.max_write=4096;respond(value);break;}
            case FUSE_LOOKUP:{assert(size==5 && !memcmp(body,"file",5));lookups++;fuse_entry_out value{};value.nodeid=9;value.attr.mode=S_IFREG|0600;respond(value);break;}
            case FUSE_OPEN:{assert(size==sizeof(fuse_open_in));fuse_open_in value;memcpy(&value,body,sizeof(value));assert(value.flags==(O_RDWR|O_DIRECT));fuse_open_out opened{};opened.fh=71;opened.open_flags=FOPEN_DIRECT_IO;respond(opened);break;}
            case FUSE_GETATTR:{fuse_attr_out value{};value.attr.size=9000;respond(value);break;}
            case FUSE_STATFS:{assert(!size && request.node==9);fuse_statfs_out value{};value.st.blocks=111;value.st.bfree=22;value.st.bavail=11;value.st.files=33;value.st.ffree=7;value.st.bsize=4096;value.st.namelen=255;value.st.frsize=4096;respond(value);break;}
            case FUSE_READ:{assert(size==sizeof(fuse_read_in));fuse_read_in value;memcpy(&value,body,sizeof(value));assert(value.fh==71 && value.flags==(O_RDWR|O_DIRECT) && value.offset==7);out={'r','e','a','d'};reads++;break;}
            case FUSE_WRITE:{assert(size>sizeof(fuse_write_in));fuse_write_in value;memcpy(&value,body,sizeof(value));assert(value.fh==71 && value.flags==(O_RDWR|O_DIRECT) && value.offset==10 && value.size==6 && size==sizeof(value)+6);fuse_write_out written{};written.size=3;respond(written);writes++;break;}
            case FUSE_FSYNC:{assert(size==sizeof(fuse_fsync_in));fuse_fsync_in value;memcpy(&value,body,sizeof(value));assert(value.fsync_flags==1 && value.fh==71);error=++syncs==1?-EIO:-ENOSYS;break;}
            case FUSE_FLUSH:error=++flushes==1?-EACCES:-ENOSYS;break;
            case FUSE_RELEASE:releases++;break;
            case FUSE_FORGET:forgets++;continue;
            default:assert(false);
            }
            ResponseHeader header{static_cast<uint32_t>(sizeof(ResponseHeader)+out.size()),error,request.unique};
            std::vector<char> result(sizeof(header)+out.size());memcpy(result.data(),&header,sizeof(header));
            if(!out.empty())memcpy(result.data()+sizeof(header),out.data(),out.size());
            assert(send(pair[1],result.data(),result.size(),MSG_NOSIGNAL)==static_cast<ssize_t>(result.size()));
        }
        close(pair[1]);
    });
    {
        Files files(channel);Files::Opened opened{};
        assert(files.open("/file",O_RDWR,opener,&opened)==-ENODEV);
        assert(files.initialize(opener)==0 && files.initialize(opener)==-EALREADY);
        assert(files.open("/../file",O_RDWR,opener,&opened)==-EOPNOTSUPP);
        assert(files.open("/file/",O_RDWR,opener,&opened)==-ENOTDIR);
        assert(files.open("/file",O_RDWR|O_APPEND,opener,&opened)==-EINVAL);
        assert(files.open("/file",O_RDWR|O_DIRECT,opener,&opened)==0 && opened.flags==FOPEN_DIRECT_IO);
        uint64_t size=0;assert(files.size(opened.slot,opened.cookie,&size)==0 && size==9000);
        fuse_kstatfs stats{};assert(files.filesystem(opened.slot,actor,&stats)==0 && stats.blocks==111 && stats.bfree==22 && stats.bavail==11 && stats.files==33 && stats.ffree==7 && stats.bsize==4096 && stats.namelen==255 && stats.frsize==4096);
        assert(files.filesystem(opened.slot+1,actor,&stats)==-EBADF);
        std::vector<char> bytes;assert(files.read(opened.slot,opened.cookie+1,0,4,actor,&bytes)==-EBADF);
        assert(files.read(opened.slot,opened.cookie,7,16,actor,&bytes)==0 && std::string(bytes.begin(),bytes.end())=="read");
        uint32_t written=0;assert(files.write(opened.slot,opened.cookie,10,{'a','b','c','d','e','f'},actor,&written)==0 && written==3);
        assert(files.sync(opened.slot,opened.cookie,2,actor)==-EINVAL);
        assert(files.sync(opened.slot,opened.cookie,1,actor)==-EIO);
        assert(files.sync(opened.slot,opened.cookie,1,actor)==0 && files.sync(opened.slot,opened.cookie,1,actor)==0);
        assert(files.flush(opened.slot,opened.cookie,actor)==-EACCES);
        assert(files.flush(opened.slot,opened.cookie,actor)==0 && files.flush(opened.slot,opened.cookie,actor)==0);
        assert(files.release(opened.slot,opened.cookie,opener)==0 && files.count()==0);
        assert(files.flush(opened.slot,opened.cookie,actor)==-EBADF && files.sync(opened.slot,opened.cookie,1,actor)==-EBADF);
    }
    channel.cancel();server.join();
    assert(reads==1 && writes==1 && syncs==2 && flushes==2 && releases==1 && lookups==2 && forgets==lookups);
    puts("FUSE_FILES_PASS original_open_flags=1 caller_identity=1 short_read=1 partial_write=1 invalid_cookie=1 real_errors_preserved=1 optional_enosys=1 statfs=1 release_forget=1");
}
