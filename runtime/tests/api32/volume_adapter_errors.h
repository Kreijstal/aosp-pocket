// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../../api32/external_volume.h"
#include <cassert>
#include <dirent.h>
using namespace pocket::fuse32;
static unsigned adapterDescriptorCount() {
    DIR* directory=opendir("/proc/self/fd");assert(directory);unsigned count=0;
    while(readdir(directory))count++;
    closedir(directory);return count;
}
static void volumeAdapterErrors(const std::string& directory) {
    const unsigned before=adapterDescriptorCount();
#ifdef __BIONIC__
    char zero[CMSG_SPACE(8*sizeof(int))]{};msghdr empty{};empty.msg_control=zero;empty.msg_controllen=sizeof(zero);
    cmsghdr* first=CMSG_FIRSTHDR(&empty);assert(first && CMSG_NXTHDR(&empty,first)==first);
#endif
    ExternalVolume::Options options;options.directory=directory;options.hostPid=getpid();assert(!kernelUid(&options.hostUid));
    // No success frame is sent, so lifetime validation must not be reached.
    options.hostStart=1;
    for(bool eof:{false,true}) {
        int listener=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);assert(listener>=0);
        sockaddr_un address{};address.sun_family=AF_UNIX;auto path=directory+"/pocket_volume32";assert(path.size()<sizeof(address.sun_path));
        memcpy(address.sun_path,path.c_str(),path.size()+1);
        assert(!bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address)) && !listen(listener,1));
        std::thread peer([&]{
            int fd=accept4(listener,nullptr,nullptr,SOCK_CLOEXEC);assert(fd>=0);VolumeRequest request{};
            assert(recv(fd,&request,sizeof(request),MSG_WAITALL)==sizeof(request));
            assert(request.magic==VolumeMagic && request.operation==2 && request.epoch==73);
            if(!eof){char byte;assert(recv(fd,&byte,1,0)==0);}
            close(fd);
        });
        const auto began=std::chrono::steady_clock::now();int result=volumeAdapter(options,2,73,eof?1000:100);
        assert(eof?result==-ECONNRESET:(result==-EAGAIN || result==-EWOULDBLOCK));
        assert(std::chrono::steady_clock::now()-began<std::chrono::seconds(5));
        peer.join();assert(!close(listener) && !unlink(path.c_str()));
    }
    assert(adapterDescriptorCount()==before);
    puts("VOLUME_ADAPTER_ERRORS32_PASS timeout_returns=1 eof_returns=1 no_ancillary_walk_on_error=1 fd_leaks=0");
}
