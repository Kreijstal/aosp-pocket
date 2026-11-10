// SPDX-License-Identifier: GPL-3.0-only
#include <lmkd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <climits>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"API32_MEMORY_PROBE_FAILED line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
namespace {
struct Children {
    pid_t pids[3]={};
    ~Children(){for(pid_t pid:pids)if(pid>0 && waitpid(pid,nullptr,WNOHANG)==0){kill(pid,SIGKILL);waitpid(pid,nullptr,0);}}
};
int connectMemory() {
    for(unsigned n=0;n<150;n++) {
        int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);if(fd<0)return -1;
        sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/lmkd");
        if(!connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))) {
            timeval timeout{5,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));return fd;
        }
        close(fd);usleep(50000);
    }
    return -1;
}
bool sendPacket(int fd,LMKD_CTRL_PACKET data,size_t size) {return send(fd,data,size,MSG_NOSIGNAL)==static_cast<ssize_t>(size);}
int count(int fd,int low,int high) {
    LMKD_CTRL_PACKET data;lmk_getkillcnt range{low,high};
    if(!sendPacket(fd,data,lmkd_pack_set_getkillcnt(data,&range)) || recv(fd,data,sizeof(data),0)!=8 ||
       lmkd_pack_get_cmd(data)!=LMK_GETKILLCNT)return -1;
    return ntohl(data[1]);
}
bool registerProcess(int fd,pid_t pid,unsigned uid,int adj,bool legacy=false) {
    LMKD_CTRL_PACKET data;lmk_procprio priority{pid,uid,adj,PROC_TYPE_APP};
    size_t size=lmkd_pack_set_procprio(data,&priority);return sendPacket(fd,data,legacy?size-4:size);
}
int kernelOom(pid_t pid) {
    char path[96];snprintf(path,sizeof(path),"/proc/%d/oom_score_adj",pid);
    FILE* file=fopen(path,"re");if(!file)return INT_MIN;
    int score=INT_MIN;if(fscanf(file,"%d",&score)!=1)score=INT_MIN;fclose(file);return score;
}
pid_t child(unsigned uid) {
    int pipeFd[2];if(pipe(pipeFd))return -1;
    pid_t pid=fork();
    if(!pid) {
        close(pipeFd[0]);if(setgid(uid) || setuid(uid) || getuid()!=uid)_exit(2);
        char ready='r';if(write(pipeFd[1],&ready,1)!=1)_exit(3);close(pipeFd[1]);
        for(;;)pause();
    }
    close(pipeFd[1]);char ready=0;ssize_t size=read(pipeFd[0],&ready,1);close(pipeFd[0]);
    if(size!=1 || ready!='r'){if(pid>0){kill(pid,SIGKILL);waitpid(pid,nullptr,0);}return -1;}
    return pid;
}
}
int main() {
    CHECK(getuid()==0 && syscall(__NR_getpid,0x504c1001UL)==0x504c1002UL);
    pid_t denied=fork();CHECK(denied>=0);
    if(!denied) {
        if(setgid(39998) || setuid(39998))_exit(2);
        int fd=connectMemory();if(fd<0)_exit(errno==EACCES || errno==EPERM?0:3);
        LMKD_CTRL_PACKET data;lmk_getkillcnt range{-1000,1000};
        sendPacket(fd,data,lmkd_pack_set_getkillcnt(data,&range));
        ssize_t got=recv(fd,data,sizeof(data),0);int error=errno;close(fd);
        _exit(got==0 || (got<0 && error==ECONNRESET)?0:4);
    }
    int status=0;CHECK(waitpid(denied,&status,0)==denied && WIFEXITED(status) && WEXITSTATUS(status)==0);
    puts("API32_MEMORY_ROLE_DENIED_PASS uid=39998 connection=closed");
    Children children;
    children.pids[0]=child(39999);children.pids[1]=child(39997);children.pids[2]=child(1000);
    for(pid_t pid:children.pids)CHECK(pid>0);
    int originalOom=kernelOom(children.pids[1]);CHECK(originalOom!=INT_MIN);
    CHECK(!setgid(1000) && !setuid(1000));int fd=connectMemory();CHECK(fd>=0);
    LMKD_CTRL_PACKET data;CHECK(sendPacket(fd,data,lmkd_pack_set_procpurge(data)) && count(fd,-1000,1000)==0);
    CHECK(sendPacket(fd,data,lmkd_pack_set_subscribe(data,LMK_ASYNC_EVENT_KILL)));
    CHECK(registerProcess(fd,children.pids[0],39998,900)); // Wrong UID: must not enroll the victim.
    CHECK(registerProcess(fd,children.pids[1],39997,500));
    CHECK(registerProcess(fd,children.pids[2],1000,950));
    // Deliberately raise the threshold above actual MemAvailable. This is an
    // isolated policy test; it does not claim the host was short of memory.
    lmk_target raised{INT_MAX,900};CHECK(sendPacket(fd,data,lmkd_pack_set_target(data,&raised,1)));
    usleep(1200000);
    for(pid_t pid:children.pids)CHECK(waitpid(pid,&status,WNOHANG)==0);
    CHECK(count(fd,-1000,1000)==0);
    CHECK(kernelOom(children.pids[1])==originalOom);
    CHECK(registerProcess(fd,children.pids[0],39999,900,true)); // Java ProcessList's four-word variant.
    CHECK(recv(fd,data,sizeof(data),0)==12 && lmkd_pack_get_cmd(data)==LMK_PROCKILL &&
        static_cast<pid_t>(ntohl(data[1]))==children.pids[0] && ntohl(data[2])==39999);
    CHECK(waitpid(children.pids[0],&status,0)==children.pids[0] && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
    children.pids[0]=0;
    CHECK(count(fd,900,900)==1 && count(fd,-1000,899)==0 && count(fd,901,1000)==0);
    CHECK(count(fd,1001,1001)==1 && count(fd,901,900)==0); // Stock total-count sentinel and empty range.
    for(pid_t pid:children.pids)if(pid>0)CHECK(waitpid(pid,&status,WNOHANG)==0);
    lmk_target disabled{0,1000};CHECK(sendPacket(fd,data,lmkd_pack_set_target(data,&disabled,1)));
    CHECK(sendPacket(fd,data,lmkd_pack_set_procpurge(data)));close(fd);
    fd=connectMemory();CHECK(fd>=0);data[0]=htonl(LMK_GETKILLCNT);data[1]=data[2]=data[3]=0;
    CHECK(sendPacket(fd,data,16));ssize_t got=recv(fd,data,sizeof(data),0);CHECK(got==0 || (got<0 && errno==ECONNRESET));close(fd);
    puts("API32_MEMORY_POLICY_PASS protocol=12L peer_roles=verified target_uid=verified thresholds=test_raised victim=owned_guest pidfd_sigkill=real kill_notification=real kill_count=real lower_priority=alive system_role=alive malformed_packet=rejected host_oom=unchanged");
    return 0;
}
