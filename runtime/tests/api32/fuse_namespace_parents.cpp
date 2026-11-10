// SPDX-License-Identifier: GPL-3.0-only
#include "fuse_namespace_peer.h"

int main() {
    char temp[]="/tmp/pocket-fuse-parents-XXXXXX";assert(mkdtemp(temp));
    Peer peer;peer.nodes.emplace(1,Peer::Node{open(temp,O_PATH|O_DIRECTORY|O_CLOEXEC)});
    assert(peer.nodes.at(1).fd>=0);
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,sockets));
    Channel channel(sockets[0],2000);std::thread server([&]{peer.run(sockets[1]);});
    {
        Files files(channel,temp);auto actor=peer.owner;fuse_attr before{};
        assert(files.stat("/../",actor,&before)==-ENODEV);
        assert(files.mkdir("/../new",0777,0027,actor)==-ENODEV);
        assert(!files.initialize(actor));
        for(auto path:{"/own","/own/child","/other"})assert(!files.mkdir(path,0777,0027,actor));
        Files::Opened base{},file{},opened{};fuse_attr attr{};
        assert(!files.open("/own/child",O_RDONLY|O_DIRECTORY,actor,&base));
        assert(!files.open("/own/child/canary",O_CREAT|O_EXCL|O_RDWR,actor,&file,0666,0027));
        assert(!files.attributes(file.slot,file.cookie,actor,&attr));auto inode=attr.ino;
        assert(!files.release(file.slot,file.cookie,actor));
        assert(!files.open("/../child/./canary",O_RDONLY,actor,&opened,0,0,&base));
        assert(!files.attributes(opened.slot,opened.cookie,actor,&attr) && attr.ino==inode);
        assert(!files.release(opened.slot,opened.cookie,actor));
        assert(!files.stat("/../child/canary",actor,&attr,&base) && attr.ino==inode);
        assert(!files.access("/../child/canary",R_OK,actor,&base));
        assert(!files.mkdir("/../sibling",0777,0027,actor,&base));
        assert(!files.rename("/../child/canary","/../sibling/moved",actor,0,&base,&base));
        assert(files.stat("/canary",actor,&attr,&base)==-ENOENT);
        assert(!files.remove("/../sibling/moved",false,actor,&base));
        assert(!files.remove("/../sibling",true,actor,&base));

        // The provider and its original directory cap did not perform this
        // move. Parent navigation must follow the held kernel inode's *new*
        // parent, not the old path or a remembered provider parent chain.
        assert(!renameat(peer.nodes.at(1).fd,"own/child",peer.nodes.at(1).fd,"other/moved"));
        assert(!files.mkdir("/other/sibling",0777,0027,actor));
        assert(!files.open("/../sibling",O_RDONLY|O_DIRECTORY,actor,&opened,0,0,&base));
        assert(!files.attributes(opened.slot,opened.cookie,actor,&attr));auto sibling=attr.ino;
        assert(!files.stat("/other/sibling",actor,&attr) && attr.ino==sibling);
        assert(!files.release(opened.slot,opened.cookie,actor));
        auto denied=actor;denied.uid=33333;
        assert(files.open("/../sibling",O_RDONLY|O_DIRECTORY,denied,&opened,0,0,&base)==-EACCES);

        // A file/symlink before ".." cannot be normalized out of the walk.
        assert(!files.open("/other/file",O_CREAT|O_EXCL|O_RDWR,actor,&file,0666,0027));
        assert(!files.release(file.slot,file.cookie,actor));
        assert(files.stat("/other/file/../sibling",actor,&attr)==-ENOTDIR);
        assert(!symlinkat("sibling",peer.nodes.at(1).fd,"other/link"));
        assert(files.stat("/other/link/../sibling",actor,&attr)==-ELOOP);
        assert(!files.remove("/other/link",false,actor));
        assert(!files.remove("/other/file",false,actor));

        assert(!files.release(base.slot,base.cookie,actor));
        assert(!files.remove("/other/moved",true,actor));
        assert(!files.remove("/other/sibling",true,actor));
        assert(!files.remove("/own",true,actor));
        assert(!files.remove("/other",true,actor));
        assert(!files.count());
    }
    channel.cancel();server.join();assert(peer.handles.empty());
    for(auto node:peer.nodes)assert(!node.second.references);
    assert(!rmdir(temp));
    puts("FUSE_NAMESPACE_PARENTS_PASS relative_open_stat_access_mutations=1 external_rename_current_parent=1 caller_denied=1 file_before_dotdot_denied=1 symlink_before_dotdot_denied=1 handles_and_lookup_refs_balanced=1");
}
