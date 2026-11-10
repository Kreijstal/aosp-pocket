// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>

// Owned by a provider node, including LOOKUP-only/O_PATH references. The
// caller must select the private Pocket view and authorize the current UID
// using MediaProvider before consulting this private inode anchor. File data
// remains inside the provider; this class never exports an FD to an app.
class PocketPrivateInodeAnchor final {
    int fd_=-1;
    dev_t device_=0;
    ino_t inode_=0;
    mode_t type_=0;
public:
    PocketPrivateInodeAnchor()=default;
    PocketPrivateInodeAnchor(const PocketPrivateInodeAnchor&)=delete;
    ~PocketPrivateInodeAnchor(){if(fd_>=0)close(fd_);}
    int initialize(const char* path,const struct stat& expected) {
        if(!S_ISDIR(expected.st_mode) && !S_ISREG(expected.st_mode))return -EOPNOTSUPP;
        if(fd_>=0)return expected.st_dev==device_ && expected.st_ino==inode_?0:-ESTALE;
        int next=open(path,O_PATH|O_NOFOLLOW|O_CLOEXEC);if(next<0)return -errno;
        struct stat actual{};int error=0;
        if(fstat(next,&actual))error=-errno;
        else if(actual.st_dev!=expected.st_dev || actual.st_ino!=expected.st_ino || (actual.st_mode&S_IFMT)!=(expected.st_mode&S_IFMT))error=-ESTALE;
        if(error){close(next);return error;}
        device_=actual.st_dev;inode_=actual.st_ino;type_=actual.st_mode&S_IFMT;fd_=next;return 0;
    }
    int unlinkedState() const {
        if(fd_<0)return 0;
        struct stat current{};if(fstat(fd_,&current))return -errno;
        if(current.st_dev!=device_ || current.st_ino!=inode_ || (current.st_mode&S_IFMT)!=type_)return -ESTALE;
        return current.st_nlink?0:1;
    }
    int verifyOpened(int descriptor) const {
        if(fd_<0)return -EAGAIN;
        struct stat current{};if(fstat(descriptor,&current))return -errno;
        return current.st_dev==device_ && current.st_ino==inode_ && (current.st_mode&S_IFMT)==type_?0:-ESTALE;
    }
    int openUnlinked(int flags) const {
        int state=unlinkedState();if(state<=0)return state?state:-EAGAIN;
        if(flags&~(O_ACCMODE|O_CLOEXEC|O_LARGEFILE|O_DIRECT|O_DIRECTORY) || (flags&O_ACCMODE)==O_ACCMODE)return -EINVAL;
        char path[64];snprintf(path,sizeof(path),"/proc/self/fd/%d",fd_);
        int result=open(path,flags|O_CLOEXEC);if(result<0)return -errno;
        int error=verifyOpened(result);
        if(error){close(result);return error;}return result;
    }
    // EAGAIN means the ordinary live-path access policy still applies.
    // An unlinked inode never acquires authority from a replacement pathname.
    int searchUnlinked() const {
        int state=unlinkedState();if(state<=0)return state?state:-EAGAIN;
        if(type_!=S_IFDIR)return -ENOTDIR;
        return faccessat(fd_,".",X_OK,0)?-errno:0;
    }
};
