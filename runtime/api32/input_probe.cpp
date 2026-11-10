// SPDX-License-Identifier: GPL-3.0-only
// Exercise the real EventHub with no host suspend capability, including death.
#include "EventHub.h"
#include "../api29/power.h"
#include <linux/capability.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <memory>
using namespace android;
namespace {
bool locks(uint64_t expected) {
    for(unsigned i=0;i<150;i++) {
        uint64_t count=0;
        if(pocketPowerRequest(3,0,nullptr,&count))return false;
        if(count==expected)return true;
        usleep(20000);
    }
    return false;
}
}
int main() {
    setbuf(stdout,nullptr);
    if(getuid()!=0 || setgid(1000) || setuid(1000))return 64;
    __user_cap_header_struct header{_LINUX_CAPABILITY_VERSION_3,0};
    __user_cap_data_struct capabilities[2]{};
    if(syscall(__NR_capget,&header,capabilities) ||
       (capabilities[CAP_BLOCK_SUSPEND/32].effective&(1U<<(CAP_BLOCK_SUSPEND%32))))return 1;
    if(!locks(0))return 2;
    // Fork before this parent opens either Binder driver. The child holds its
    // own remote IWakeLock, and SIGKILL must cause the server to release it.
    int ready[2];if(pipe(ready))return 3;
    pid_t child=fork();if(child<0)return 4;
    if(!child) {
        close(ready[0]);auto hub=std::make_unique<EventHub>();
        char byte='1';if(write(ready[1],&byte,1)!=1)_exit(5);
        close(ready[1]);for(;;)pause();
    }
    close(ready[1]);pollfd descriptor{ready[0],POLLIN,0};char byte=0;
    bool held=poll(&descriptor,1,10000)==1 && read(ready[0],&byte,1)==1 && byte=='1' && locks(1);
    close(ready[0]);kill(child,SIGKILL);
    int status;while(waitpid(child,&status,0)<0)if(errno!=EINTR)return 6;
    if(!held || !WIFSIGNALED(status) || WTERMSIG(status)!=SIGKILL || !locks(0))return 7;
    puts("API32_INPUT_WAKE_DEATH_PASS host_lock=held client_sigkill=1 host_lock_released=1");
    auto hub=std::make_unique<EventHub>();
    if(!locks(1))return 8;
    RawEvent events[32];size_t count=hub->getEvents(0,events,32);
    if(count>32)return 9;
    hub->wake();hub->getEvents(0,events,32);
    hub.reset();if(!locks(0))return 10;
    puts("API32_INPUT_PASS stock_reader=12L eventhub_poll=1 wake_pipe=1 host_lock_lifecycle=1 kernel_cap_block_suspend=0");
    return 0;
}
