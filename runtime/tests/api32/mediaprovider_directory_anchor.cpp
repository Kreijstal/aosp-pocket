// SPDX-License-Identifier: GPL-3.0-only
#include "../../api32/mediaprovider_directory_anchor.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>

static unsigned descriptors() {
    DIR* directory=opendir("/proc/self/fd");assert(directory);unsigned count=0;
    while(readdir(directory))count++;
    closedir(directory);return count;
}
int main() {
    unsigned before=descriptors();char root[]="/tmp/pocket-provider-anchor-XXXXXX";assert(mkdtemp(root));
    std::string path=std::string(root)+"/held",moved=std::string(root)+"/moved",link=std::string(root)+"/link";
    assert(!mkdir(path.c_str(),0700));struct stat expected{};assert(!stat(path.c_str(),&expected));
    int witness=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(witness>=0);
    {
        PocketPrivateInodeAnchor anchor;
        assert(anchor.searchUnlinked()==-EAGAIN);
        struct stat wrong=expected;wrong.st_ino++;
        assert(anchor.initialize(path.c_str(),wrong)==-ESTALE);
        assert(!anchor.initialize(path.c_str(),expected));
        assert(descriptors()==before+2); // One witness and one O_PATH anchor.
        assert(anchor.searchUnlinked()==-EAGAIN);
        assert(!rename(path.c_str(),moved.c_str()));
        assert(!anchor.initialize(moved.c_str(),expected));
        assert(!rmdir(moved.c_str()));
        assert(!anchor.searchUnlinked());
        int fresh=anchor.openUnlinked(O_RDONLY|O_DIRECTORY);assert(fresh>=0 && !anchor.verifyOpened(fresh));
        DIR* empty=fdopendir(fresh);assert(empty);unsigned entries=0;while(readdir(empty))entries++;assert(!entries);closedir(empty);
        assert(!fchmod(witness,0600));assert(anchor.searchUnlinked()==-EACCES);
        assert(!fchmod(witness,0400));fresh=anchor.openUnlinked(O_RDONLY|O_DIRECTORY);assert(fresh>=0);close(fresh);
        assert(anchor.searchUnlinked()==-EACCES);assert(!fchmod(witness,0600));
        assert(!mkdir(moved.c_str(),0700));struct stat replacement{};assert(!stat(moved.c_str(),&replacement) && replacement.st_ino!=expected.st_ino);
        assert(anchor.initialize(moved.c_str(),replacement)==-ESTALE);
        assert(anchor.searchUnlinked()==-EACCES); // Replacement grants nothing.
        assert(!fchmod(witness,0700));assert(!anchor.searchUnlinked());
        assert(!rmdir(moved.c_str()));
        assert(!symlink(root,link.c_str()));
        struct stat target{};assert(!stat(root,&target));PocketPrivateInodeAnchor nofollow;
        assert(nofollow.initialize(link.c_str(),target)==-ESTALE);assert(!unlink(link.c_str()));
    }
    assert(descriptors()==before+1);assert(!close(witness));
    std::string file=std::string(root)+"/file";int original=open(file.c_str(),O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);assert(original>=0);
    assert(write(original,"original",8)==8);assert(!fstat(original,&expected));
    {
        PocketPrivateInodeAnchor anchor;assert(!anchor.initialize(file.c_str(),expected));assert(!unlink(file.c_str()));
        int replacement=open(file.c_str(),O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);assert(replacement>=0 && write(replacement,"new",3)==3);
        assert(anchor.verifyOpened(replacement)==-ESTALE);assert(!close(replacement));
        int fresh=anchor.openUnlinked(O_RDONLY);assert(fresh>=0 && !anchor.verifyOpened(fresh));char data[16]{};
        assert(read(fresh,data,sizeof(data))==8 && !memcmp(data,"original",8));assert(lseek(original,0,SEEK_CUR)==8);close(fresh);
        assert(anchor.searchUnlinked()==-ENOTDIR);
        assert(anchor.openUnlinked(O_RDONLY|O_DIRECTORY)==-ENOTDIR);
        assert(anchor.openUnlinked(O_RDWR|O_TRUNC)==-EINVAL);
        assert(!fchmod(original,0));assert(anchor.openUnlinked(O_RDONLY)==-EACCES);
        assert(pread(original,data,sizeof(data),0)==8);assert(!fchmod(original,0600));
        fresh=anchor.openUnlinked(O_RDWR);assert(fresh>=0 && pwrite(fresh,"X",1,0)==1);close(fresh);
        assert(pread(original,data,1,0)==1 && data[0]=='X');assert(!unlink(file.c_str()));
    }
    assert(!close(original));assert(!rmdir(root));assert(descriptors()==before);
    puts("MEDIAPROVIDER_DIRECTORY_ANCHOR_PASS inode_type_verified=1 nofollow=1 lookup_only_anchor=1 held_after_rename_unlink=1 live_paths_delegated=1 execute_revocation=1 replacement_not_reopened=1 final_reference_closes=1 fd_leaks=0 policy_not_exercised=1");
}
