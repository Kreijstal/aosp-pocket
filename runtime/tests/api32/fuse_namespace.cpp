// SPDX-License-Identifier: GPL-3.0-only
#include "fuse_namespace_peer.h"

int main() {
    char temp[]="/tmp/pocket-fuse-namespace-XXXXXX";assert(mkdtemp(temp));
    Peer peer;peer.nodes.emplace(1,Peer::Node{open(temp,O_PATH|O_DIRECTORY|O_CLOEXEC)});assert(peer.nodes.at(1).fd>=0);
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,sockets));Channel channel(sockets[0],2000);
    std::thread server([&]{peer.run(sockets[1]);});
    {
        Files files(channel);auto actor=peer.owner;Files::Opened file{},directory{};fuse_attr st{};
        assert(files.mkdir("/a",0777,0027,actor)==-ENODEV);
        assert(!files.initialize(actor));
        assert(!files.mkdir("/a",0777,0027,actor) && !files.mkdir("/b",0777,0027,actor));
        assert(files.mkdir("/a",0777,0027,actor)==-EEXIST);
        assert(files.mkdir("/missing/child",0777,0027,actor)==-ENOENT);
        assert(files.remove("/",true,actor)==-EBUSY);
        // Parent navigation needs the broker's independently authenticated
        // lower anchors; this transport-only peer does not configure them.
        assert(files.mkdir("/a/../escape",0777,0027,actor)==-EOPNOTSUPP);
        assert(files.open("/a/new",O_CREAT|O_EXCL|O_RDWR,actor,&file,0666,0027)==0);
        uint32_t written=0;assert(!files.write(file.slot,file.cookie,0,{'d','a','t','a'},actor,&written) && written==4);
        assert(!files.stat("/a/new",actor,&st) && st.size==4 && (st.mode&0777)==0640);
        auto inode=st.ino;
        Files::Opened unused{};assert(files.open("/a/new",O_CREAT|O_EXCL|O_RDWR,actor,&unused,0666,0027)==-EEXIST);
        assert(files.open("/a/new/child",O_CREAT|O_RDWR,actor,&unused,0666,0027)==-ENOTDIR);
        assert(files.open("/a/new",O_RDONLY|O_DIRECTORY,actor,&unused)==-ENOTDIR);
        assert(files.open("/a",O_WRONLY,actor,&unused)==-EISDIR);
        assert(!files.open("/a",O_RDONLY|O_DIRECTORY,actor,&directory) && directory.directory && !file.directory);
        std::vector<Files::DirectoryEntry> entries;assert(!files.list(directory.slot,directory.cookie,0,4096,actor,&entries));
        assert(entries.size()==1 && entries[0].name=="new" && entries[0].attributes.ino==inode && entries[0].attributes.size==4);
        uint64_t end=entries[0].offset;assert(!files.list(directory.slot,directory.cookie,end,4096,actor,&entries) && entries.empty());
        assert(!files.list(directory.slot,directory.cookie,0,4096,actor,&entries) && entries.size()==1);
        assert(files.list(file.slot,file.cookie,0,4096,actor,&entries)==-ENOTDIR);
        assert(files.list(directory.slot,directory.cookie+1,0,4096,actor,&entries)==-EBADF);
        assert(files.list(directory.slot,directory.cookie,0,32,actor,&entries)==-EINVAL);
        // The server applies a real cross-directory rename. The old path is
        // absent, stat resolves the new inode, and the original handle lives.
        assert(!files.rename("/a/new","/b/moved",actor));
        assert(files.stat("/a/new",actor,&st)==-ENOENT);
        assert(!files.stat("/b/moved",actor,&st) && st.ino==inode && st.size==4);
        assert(!files.attributes(file.slot,file.cookie,actor,&st) && st.ino==inode);
        assert(!files.list(directory.slot,directory.cookie,0,4096,actor,&entries) && entries.empty());
        assert(files.rename("/b/moved","/a/moved",actor,1)==-EINVAL); // Provider error, no emulation.
        assert(files.remove("/b",true,actor)==-ENOTEMPTY);
        assert(!files.remove("/b/moved",false,actor));
        assert(files.stat("/b/moved",actor,&st)==-ENOENT);
        std::vector<char> bytes;assert(!files.read(file.slot,file.cookie,0,4,actor,&bytes) && std::string(bytes.begin(),bytes.end())=="data");
        assert(!files.release(file.slot,file.cookie,actor) && !files.release(directory.slot,directory.cookie,actor));
        // Nonexclusive create must recover from LOOKUP/CREATE racing an
        // independent creator, then preserve the file on a subsequent open.
        peer.race=true;assert(!files.open("/a/race",O_CREAT|O_RDWR,actor,&file,0666,0027));
        assert(!files.write(file.slot,file.cookie,0,{'x'},actor,&written) && written==1);
        assert(!files.release(file.slot,file.cookie,actor));
        assert(!files.open("/a/race",O_CREAT|O_RDWR,actor,&file,0666,0027));
        uint64_t size=0;assert(!files.size(file.slot,file.cookie,&size) && size==1);assert(!files.release(file.slot,file.cookie,actor));
        assert(!files.open("/a/race",O_TRUNC|O_RDWR,actor,&file));
        assert(!files.size(file.slot,file.cookie,&size) && !size);assert(!files.release(file.slot,file.cookie,actor));
        // Lower-view changes are not hidden by cached lookup/attributes.
        int lower=openat(peer.nodes.at(1).fd,"a/race",O_WRONLY|O_CLOEXEC);assert(lower>=0 && write(lower,"external",8)==8);close(lower);
        assert(!files.stat("/a/race",actor,&st) && st.size==8);
        assert(symlinkat("../b",peer.nodes.at(1).fd,"a/link")==0);
        assert(files.stat("/a/link/escape",actor,&st)==-ELOOP);
        assert(!files.remove("/a/link",false,actor));
        auto denied=actor;denied.uid=33333;assert(files.remove("/a/race",false,denied)==-EACCES);
        assert(!files.stat("/a/race",actor,&st));
        assert(!files.open("/a/second",O_CREAT|O_EXCL|O_RDWR,actor,&file,0666,0027));
        assert(!files.release(file.slot,file.cookie,actor));
        assert(!files.open("/a",O_RDONLY|O_DIRECTORY,actor,&directory));
        assert(!files.list(directory.slot,directory.cookie,0,160,actor,&entries) && entries.size()==1);
        auto first=entries[0];assert(!files.list(directory.slot,directory.cookie,first.offset,160,actor,&entries) && entries.size()==1 && entries[0].name!=first.name);
        peer.badDirectory=true;assert(files.list(directory.slot,directory.cookie,0,4096,actor,&entries)==-EPROTO && entries.empty());
        assert(!files.list(directory.slot,directory.cookie,0,4096,actor,&entries) && entries.size()==2);
        assert(!files.release(directory.slot,directory.cookie,actor));
        assert(!files.remove("/a/second",false,actor));
        // Relative opens retain the base's LOOKUP references independently of
        // its OPENDIR handle. Rename and unlink change the live filesystem;
        // held nodes still work, without reconstructing the original path.
        Files::Opened base{},child{};
        assert(!files.open("/b",O_RDONLY|O_DIRECTORY,actor,&base));
        assert(!files.mkdir("/relative",0777,0027,actor,&base));
        assert(!files.open("/relative",O_RDONLY|O_DIRECTORY,actor,&child,0,0,&base));
        assert(!files.release(base.slot,base.cookie,actor));
        assert(!files.rename("/b","/moved-base",actor));
        assert(!files.stat("/",actor,&st,&child) && S_ISDIR(st.mode));
        assert(!files.remove("/moved-base/relative",true,actor));
        assert(!files.stat("/",actor,&st,&child) && !st.nlink);
        assert(files.mkdir("/gone",0777,0027,actor,&child)==-ENOENT);
        assert(!files.release(child.slot,child.cookie,actor));
        assert(!files.rename("/moved-base","/b",actor));
        assert(!files.remove("/a/race",false,actor));assert(!files.remove("/a",true,actor) && !files.remove("/b",true,actor));
        assert(files.count()==0);
    }
    channel.cancel();server.join();
    assert(peer.handles.empty() && peer.creates==3 && peer.denials==1 && peer.releases==9);
    for(auto n:peer.nodes)assert(n.second.references==0);
    assert(rmdir(temp)==0);
    puts("FUSE_NAMESPACE_PASS real_files=1 create_exclusive_race=1 umask=1 truncate=1 directory_seek=1 cross_directory_rename=1 unlinked_handle=1 fresh_attributes=1 symlink_denied=1 caller_denied=1 malformed_directory=1 relative_parent_lookup_lifetime=1 held_renamed_unlinked_directory=1 handles_and_lookup_refs_balanced=1");
}
