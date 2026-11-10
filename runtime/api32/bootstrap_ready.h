// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

// The trusted startup service writes this staged nonce, then exits. The init
// owner calls this only after reaping that service, so no wrapper remains when
// the Manager observes readiness. Both paths live in the same guest directory.
inline bool pocketPublishBootReady32(const char* run,const char* staged,const char* ready) {
    if(!run)return false;
    size_t size=strnlen(run,65);if(!size || size>64)return false;
    for(size_t i=0;i<size;i++)if(!((run[i]>='a' && run[i]<='z') || (run[i]>='A' && run[i]<='Z') || (run[i]>='0' && run[i]<='9') || run[i]=='-'))return false;
    int fd=open(staged,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return false;
    struct stat st{};char bytes[66];size_t count=0;bool valid=!fstat(fd,&st) && S_ISREG(st.st_mode);
    while(valid && count<sizeof(bytes)) {
        ssize_t n=read(fd,bytes+count,sizeof(bytes)-count);
        if(n<0 && errno==EINTR)continue;
        if(n<0){valid=false;break;}if(!n)break;count+=n;
    }
    close(fd);
    return valid && count==size+1 && bytes[size]=='\n' && !memcmp(bytes,run,size) && !rename(staged,ready);
}
