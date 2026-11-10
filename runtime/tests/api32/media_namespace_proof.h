// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Called only after a real stock MediaProvider session has completed INIT.
// The registered guest app owns this Android/media directory. Shared-volume
// MediaStore publication and production path/dirfd routing are separate gates.
static void mediaNamespaceProof(pocket::fuse32::Files& files,pocket::fuse32::Identity actor,const std::string& package,const std::string& nonce) {
    using namespace pocket::fuse32;
    std::string base="/0/Android/media/"+package+"/namespace-"+nonce;
    auto check=[&](const char* step,int result){printf("MEDIA_NAMESPACE32_STEP %s result=%d\n",step,result);REQUIRE(result==0);};
    check("mkdir",files.mkdir(base,0777,0027,actor));
    check("mkdir-from",files.mkdir(base+"/from",0777,0027,actor));
    check("mkdir-to",files.mkdir(base+"/to",0777,0027,actor));
    Files::Opened file{},directory{},unused{};
    std::string from=base+"/from/canary.bin",to=base+"/to/moved.bin";
    check("create",files.open(from,O_CREAT|O_EXCL|O_RDWR,actor,&file,0666,0027));
    REQUIRE(files.open(from,O_CREAT|O_EXCL|O_RDWR,actor,&unused,0666,0027)==-EEXIST);
    std::string canary="namespace-data-"+nonce;std::vector<char> data(canary.begin(),canary.end());uint32_t written=0;
    check("write",files.write(file.slot,file.cookie,0,data,actor,&written));REQUIRE(written==data.size());
    check("sync",files.sync(file.slot,file.cookie,0,actor));
    fuse_attr before{};check("stat",files.stat(from,actor,&before));REQUIRE(S_ISREG(before.mode) && before.size==data.size());
    check("opendir",files.open(base+"/from",O_RDONLY|O_DIRECTORY,actor,&directory));
    std::vector<Files::DirectoryEntry> entries;check("readdirplus",files.list(directory.slot,directory.cookie,0,4096,actor,&entries));
    auto found=std::find_if(entries.begin(),entries.end(),[](const auto& e){return e.name=="canary.bin";});
    REQUIRE(found!=entries.end() && found->attributes.ino==before.ino && found->attributes.size==data.size());
    uint64_t end=entries.back().offset;check("readdir-eof",files.list(directory.slot,directory.cookie,end,4096,actor,&entries));REQUIRE(entries.empty());
    check("releasedir",files.release(directory.slot,directory.cookie,actor));
    check("rename",files.rename(from,to,actor));
    fuse_attr after{};REQUIRE(files.stat(from,actor,&after)==-ENOENT);
    check("stat-renamed",files.stat(to,actor,&after));REQUIRE(after.ino==before.ino && after.size==before.size);
    check("unlink-open",files.remove(to,false,actor));REQUIRE(files.stat(to,actor,&after)==-ENOENT);
    std::vector<char> read;check("read-unlinked",files.read(file.slot,file.cookie,0,data.size(),actor,&read));REQUIRE(read==data);
    check("release-unlinked",files.release(file.slot,file.cookie,actor));
    check("create-truncate-canary",files.open(from,O_CREAT|O_EXCL|O_RDWR,actor,&file,0666,0027));
    check("write-truncate-canary",files.write(file.slot,file.cookie,0,data,actor,&written));REQUIRE(written==data.size());
    check("release-truncate-canary",files.release(file.slot,file.cookie,actor));
    check("open-truncate",files.open(from,O_TRUNC|O_RDWR,actor,&file));
    uint64_t size=1;check("size-truncated",files.size(file.slot,file.cookie,&size));REQUIRE(size==0);
    check("release-truncated",files.release(file.slot,file.cookie,actor));
    check("unlink-truncated",files.remove(from,false,actor));
    check("rmdir-from",files.remove(base+"/from",true,actor));
    check("rmdir-to",files.remove(base+"/to",true,actor));
    check("rmdir",files.remove(base,true,actor));REQUIRE(files.count()==0);
    int denied=files.mkdir("/0/Android/data/"+package+".other/namespace-"+nonce,0777,0027,actor);
    REQUIRE(denied==-EACCES || denied==-ENOENT || denied==-EPERM);
    puts("MEDIA_NAMESPACE32_PASS provider=real create=1 exclusive=1 caller_umask=forwarded readdirplus=1 rename=1 unlink_open_handle=1 truncate=1 rmdir=1 cross_private_denied=1 remaining_handles=0");
}
