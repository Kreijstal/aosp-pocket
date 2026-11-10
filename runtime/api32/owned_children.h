// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <set>

// Unprivileged Linux child ownership, without PID 1 or a PID namespace.
// Keep wait/reap and signal operations on the supervisor's main thread: a
// direct child's PID cannot be reused until this process reaps that child.
inline int pocketBecomeSubreaper32() {
    int enabled=0;
    if(prctl(PR_SET_CHILD_SUBREAPER,1) || prctl(PR_GET_CHILD_SUBREAPER,&enabled))return -errno;
    return enabled==1?0:-EIO;
}

// /proc supplies only candidate PIDs. waitid certifies the actual kernel
// parent relationship, including children forked by other supervisor threads.
// WNOWAIT pins exited children until the supervisor's explicit reap. No guest
// UID label, executable prefix or readable per-process metadata grants ownership.
inline int pocketOwnedChildren32(std::set<pid_t>* children) {
    children->clear();DIR* processes=opendir("/proc");if(!processes)return -errno;
    int error=0;
    for(;;) {
        errno=0;dirent* process=readdir(processes);
        if(!process){if(errno)error=-errno;break;}
        char* end=nullptr;errno=0;long pid=strtol(process->d_name,&end,10);
        if(errno || !end || *end || pid<=1 || pid>INT_MAX)continue;
        siginfo_t info{};int result;
        do{result=waitid(P_PID,static_cast<id_t>(pid),&info,WEXITED|WNOHANG|WNOWAIT);}while(result && errno==EINTR);
        if(result){if(errno==ECHILD || errno==ESRCH)continue;error=-errno;break;}
        if(children->size()>=4096){error=-E2BIG;break;}
        children->insert(static_cast<pid_t>(pid));
    }
    closedir(processes);return error;
}

inline int pocketSignalAdopted32(const std::set<pid_t>& services,int signal,
                                std::set<pid_t>* signalled=nullptr) {
    std::set<pid_t> children;int error=pocketOwnedChildren32(&children);if(error)return error;
    for(pid_t pid:children) {
        if(services.count(pid))continue;
        if(kill(pid,signal)) {if(errno!=ESRCH)return -errno;}
        else if(signalled)signalled->insert(pid);
    }
    return 0;
}
