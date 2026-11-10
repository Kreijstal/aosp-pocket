// SPDX-License-Identifier: GPL-3.0-only
// Rootless guest memory policy using the pinned 12L LMK wire protocol.
// Only this profile's authenticated app processes are eligible. Host OOM
// scores, cgroups, PSI triggers and other applications remain host-owned.
#include <lmkd.h>
#include "../api28/binder_identity.h"
#include "../direct/kernel_identity.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <chrono>
#include <climits>
#include <map>
#include <vector>

namespace {
struct Process { unsigned uid;int adj;uint64_t start; };
struct Client { int fd;pid_t pid;unsigned events=0; };
std::map<pid_t,Process> processes;
std::map<int,unsigned> kills;
std::vector<lmk_target> targets;
std::vector<Client> clients;
uid_t kernelUid;

// Both the kernel app UID and a matching logical identity/start-time record
// are required. A process in another Pocket profile has no record here.
bool identity(pid_t pid,unsigned* uid,uint64_t* start) {
    uint64_t logical=0;if(!pocket_binder_identity(pid,&logical))return false;
    char path[96],line[4096];unsigned actual=UINT_MAX;
    snprintf(path,sizeof(path),"/proc/%d/status",pid);FILE* file=fopen(path,"re");
    if(!file)return false;
    while(fgets(line,sizeof(line),file))if(sscanf(line,"Uid: %u",&actual)==1)break;
    fclose(file);if(actual!=kernelUid)return false;
    snprintf(path,sizeof(path),"/proc/%d/stat",pid);file=fopen(path,"re");if(!file)return false;
    size_t count=fread(line,1,sizeof(line),file);fclose(file);
    *start=pocket_process_start(line,count);*uid=logical>>32;
    const char* end=static_cast<const char*>(memrchr(line,')',count));
    return *start && end && end+2<line+count && end[2]!='Z' && end[2]!='X';
}
bool systemPeer(pid_t pid) {
    unsigned uid;uint64_t start;
    return identity(pid,&uid,&start) && (uid==0 || uid==1000);
}
bool packet(Client& client,const LMKD_CTRL_PACKET input,size_t bytes) {
    const size_t words=bytes/sizeof(int);LMKD_CTRL_PACKET data;memcpy(data,input,bytes);
    auto command=lmkd_pack_get_cmd(data);
    switch(command) {
    case LMK_TARGET: {
        if(words<3 || words>MAX_TARGETS*2+1 || !(words%2))return false;
        std::vector<lmk_target> next;
        for(size_t at=0;at<(words-1)/2;at++) {
            lmk_target target;lmkd_pack_get_target(data,at,&target);
            if(target.minfree<0 || target.oom_adj_score< -1000 || target.oom_adj_score>1000 ||
               (!next.empty() && (target.minfree<next.back().minfree || target.oom_adj_score<next.back().oom_adj_score)))return false;
            next.push_back(target);
        }
        targets=std::move(next);
        fprintf(stderr,"API32_MEMORY_TARGETS count=%zu source=host_MemAvailable page_unit=4096\n",targets.size());return true;
    }
    case LMK_PROCPRIO: {
        if(words!=4 && words!=5)return false;
        lmk_procprio priority;lmkd_pack_get_procprio(data,words-1,&priority);
        if(priority.pid<=0 || priority.oomadj< -1000 || priority.oomadj>1000 ||
           priority.ptype<PROC_TYPE_FIRST || priority.ptype>=PROC_TYPE_COUNT)return false;
        unsigned uid;uint64_t start;
        if(!identity(priority.pid,&uid,&start) || uid!=priority.uid) {
            fprintf(stderr,"API32_MEMORY_TARGET_REJECTED pid=%d claimed_uid=%u\n",priority.pid,priority.uid);
            return true; // A process can exit between framework bookkeeping and this one-way message.
        }
        if(processes.size()<4096 || processes.count(priority.pid))
            processes[priority.pid]={uid,priority.oomadj,start};
        return true;
    }
    case LMK_PROCREMOVE:
        if(words!=2)return false;
        processes.erase(static_cast<pid_t>(ntohl(data[1])));return true;
    case LMK_PROCPURGE:
        if(words!=1)return false;
        processes.clear();return true;
    case LMK_GETKILLCNT: {
        if(words!=3)return false;
        lmk_getkillcnt range;lmkd_pack_get_getkillcnt(data,&range);
        if(range.min_oomadj< -1000 || range.max_oomadj< -1000)return false;
        uint64_t total=0;for(const auto& value:kills)
            if(range.min_oomadj<=range.max_oomadj && (range.min_oomadj>1000 ||
               (value.first>=range.min_oomadj && value.first<=range.max_oomadj)))total+=value.second;
        const size_t size=lmkd_pack_set_getkillcnt_repl(data,std::min<uint64_t>(total,INT_MAX));
        return send(client.fd,data,size,MSG_NOSIGNAL|MSG_DONTWAIT)==static_cast<ssize_t>(size);
    }
    case LMK_SUBSCRIBE: {
        if(words!=2)return false;
        lmk_subscribe subscription;lmkd_pack_get_subscribe(data,&subscription);
        if(subscription.evt_type<LMK_ASYNC_EVENT_FIRST || subscription.evt_type>=LMK_ASYNC_EVENT_COUNT)return false;
        client.events|=1U<<subscription.evt_type;return true;
    }
    case LMK_UPDATE_PROPS: {
        if(words!=1)return false;
        size_t size=lmkd_pack_set_update_props_repl(data,-EOPNOTSUPP);
        return send(client.fd,data,size,MSG_NOSIGNAL|MSG_DONTWAIT)==static_cast<ssize_t>(size);
    }
    default:return false;
    }
}
void pressure() {
    if(targets.empty() || processes.empty())return;
    FILE* file=fopen("/proc/meminfo","re");if(!file)return;
    char line[256];uint64_t available=UINT64_MAX;
    while(fgets(line,sizeof(line),file)) {
        unsigned long long kb;
        if(sscanf(line,"MemAvailable: %llu kB",&kb)==1 && kb<=UINT64_MAX/1024){available=kb*1024;break;}
    }
    fclose(file);if(available==UINT64_MAX)return;
    int minimum=1001;
    // ProcessList.java in this platform specifies 4096-byte wire pages.
    for(const auto& target:targets)if(available<static_cast<uint64_t>(target.minfree)*4096) {
        minimum=std::max(0,target.oom_adj_score);break;
    }
    auto victim=processes.end();
    for(auto it=processes.begin();it!=processes.end();) {
        unsigned uid;uint64_t start;
        if(!identity(it->first,&uid,&start) || uid!=it->second.uid || start!=it->second.start) {
            it=processes.erase(it);continue;
        }
        // Preserve logical system daemons and persistent processes. Among
        // eligible apps use the framework's current importance ordering.
        if(uid>=10000 && it->second.adj>=minimum &&
           (victim==processes.end() || it->second.adj>victim->second.adj))victim=it;
        ++it;
    }
    if(victim==processes.end())return;
    pid_t pid=victim->first;Process process=victim->second;
    int descriptor=syscall(__NR_pidfd_open,pid,0);if(descriptor<0)return;
    unsigned uid;uint64_t start;
    bool current=identity(pid,&uid,&start) && uid==process.uid && start==process.start;
    bool killed=current && syscall(__NR_pidfd_send_signal,descriptor,SIGKILL,nullptr,0)==0;
    close(descriptor);
    if(!killed)return;
    kills[process.adj]++;processes.erase(pid);
    fprintf(stderr,"API32_MEMORY_KILL pid=%d guest_uid=%u adj=%d available_bytes=%llu source=host_MemAvailable signal=pidfd host_oom=unchanged\n",
        pid,uid,process.adj,static_cast<unsigned long long>(available));
    LMKD_CTRL_PACKET data;size_t size=lmkd_pack_set_prockills(data,pid,uid);
    for(auto& client:clients)if(client.events&(1U<<LMK_ASYNC_EVENT_KILL))
        if(send(client.fd,data,size,MSG_NOSIGNAL|MSG_DONTWAIT)!=static_cast<ssize_t>(size))shutdown(client.fd,SHUT_RDWR);
    // No kernel PSI/cgroup telemetry or statsd kill payload is manufactured.
}
}
int main(int argc,char**) {
    char sdk[PROP_VALUE_MAX];__system_property_get("ro.build.version.sdk",sdk);
    if(argc!=1 || strcmp(sdk,"32") || getuid()!=0 || syscall(__NR_getpid,0x504c1001UL)!=0x504c1002UL)return 64;
    kernelUid=pocket_kernel_uid_from_proc();if(kernelUid==static_cast<uid_t>(-1))return 65;
    const char* inherited=getenv("ANDROID_SOCKET_lmkd");if(!inherited)return 66;
    char* end=nullptr;long fd=strtol(inherited,&end,10);if(!end || *end || fd<0 || fd>INT_MAX)return 66;
    int server=fd,type=0;socklen_t length=sizeof(type);sockaddr_un address{};socklen_t addressLength=sizeof(address);
    if(getsockopt(server,SOL_SOCKET,SO_TYPE,&type,&length) || type!=SOCK_SEQPACKET ||
       getsockname(server,reinterpret_cast<sockaddr*>(&address),&addressLength) ||
       address.sun_family!=AF_UNIX || strcmp(address.sun_path,"/dev/socket/lmkd") || listen(server,8))return 67;
    fcntl(server,F_SETFD,FD_CLOEXEC);fcntl(server,F_SETFL,O_NONBLOCK);
    puts("API32_MEMORY_READY protocol=12L scope=owned_guest_apps kernel_oom=unchanged pressure=host_MemAvailable");fflush(stdout);
    auto lastPressure=std::chrono::steady_clock::now();
    for(;;) {
        std::vector<pollfd> pollers{{server,POLLIN,0}};
        for(const auto& client:clients)pollers.push_back({client.fd,POLLIN,0});
        int ready=poll(pollers.data(),pollers.size(),200);if(ready<0 && errno!=EINTR)return 68;
        for(size_t n=clients.size();n>0;--n)if(pollers[n].revents) {
            auto& client=clients[n-1];LMKD_CTRL_PACKET data;
            ssize_t bytes=recv(client.fd,data,sizeof(data),MSG_TRUNC|MSG_DONTWAIT);
            bool valid=bytes>0 && bytes<=static_cast<ssize_t>(sizeof(data)) && bytes%sizeof(int)==0 &&
                systemPeer(client.pid) && packet(client,data,bytes);
            if(!valid){close(client.fd);clients.erase(clients.begin()+n-1);}
        }
        if(pollers[0].revents&POLLIN) {
            int client=accept4(server,nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK);
            if(client>=0) {
                ucred peer{};socklen_t size=sizeof(peer);
                if(clients.size()<8 && !getsockopt(client,SOL_SOCKET,SO_PEERCRED,&peer,&size) &&
                   size==sizeof(peer) && peer.uid==kernelUid && systemPeer(peer.pid)) {
                    clients.push_back({client,peer.pid,0});
                    fprintf(stderr,"API32_MEMORY_CLIENT pid=%d role=system authenticated=1\n",peer.pid);
                } else {close(client);fprintf(stderr,"API32_MEMORY_PEER_DENIED pid=%d\n",peer.pid);}
            }
        }
        auto now=std::chrono::steady_clock::now();
        if(now-lastPressure>=std::chrono::seconds(1)){pressure();lastPressure=now;}
    }
}
