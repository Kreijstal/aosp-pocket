// SPDX-License-Identifier: GPL-3.0-only
#include "../../api32/owned_children.h"
#include <sys/wait.h>
#include <poll.h>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace {
void check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
struct Message {int kind;pid_t pid;};
void report(int fd,int kind,pid_t pid) {
    Message message{kind,pid};if(write(fd,&message,sizeof(message))!=sizeof(message))_exit(72);
}
Message receive(int fd) {
    pollfd ready{fd,POLLIN,0};check(poll(&ready,1,5000)==1,"child readiness timed out");
    Message message{};check(read(fd,&message,sizeof(message))==sizeof(message),"short child readiness");return message;
}
int wait(pid_t pid) {
    auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);int status=0;
    for(;;) {
        pid_t done=waitpid(pid,&status,WNOHANG);if(done==pid)return status;
        check(done==0 || errno==EINTR,"cannot reap child");
        check(std::chrono::steady_clock::now()<until,"child did not stop");usleep(1000);
    }
}
void cleanup() {
    for(unsigned attempt=0;attempt<1000;attempt++) {
        std::set<pid_t> children;if(pocketOwnedChildren32(&children) || children.empty())return;
        for(pid_t pid:children)kill(pid,SIGKILL);
        while(waitpid(-1,nullptr,WNOHANG)>0){}
        usleep(1000);
    }
}
}

int main() {
    std::thread worker;std::atomic<bool> release{false};int channel[2]={-1,-1};int result=1;
    try {
        check(getuid()!=0,"run the ownership fixture without root");
        check(pocketBecomeSubreaper32()==0,"cannot enable unprivileged subreaper");
        check(pipe(channel)==0,"pipe failed");
        pid_t service=fork();check(service>=0,"service fork failed");
        if(!service){for(;;)pause();}
        std::set<pid_t> services{service};
        pid_t intermediate=fork();check(intermediate>=0,"daemon parent fork failed");
        if(!intermediate) {
            if(setsid()<0)_exit(70);
            pid_t daemon=fork();if(daemon<0)_exit(71);
            if(!daemon) {
                if(setsid()<0)_exit(73);
                struct sigaction ignore{};ignore.sa_handler=SIG_IGN;sigemptyset(&ignore.sa_mask);
                if(sigaction(SIGTERM,&ignore,nullptr))_exit(74);
                report(channel[1],2,getpid());for(;;)pause();
            }
            report(channel[1],1,daemon);_exit(0);
        }
        Message first=receive(channel[0]),second=receive(channel[0]);
        check(first.pid==second.pid && first.pid>1 && first.kind+second.kind==3,"wrong daemon readiness");
        pid_t daemon=first.pid;check(wait(intermediate)==0,"daemon parent failed");
        check(getpgid(daemon)==daemon,"daemon did not escape the service process group");
        std::set<pid_t> children;
        check(pocketOwnedChildren32(&children)==0 && children==std::set<pid_t>({service,daemon}),"orphan was not adopted by the supervisor");
        std::set<pid_t> signalled;
        check(pocketSignalAdopted32(services,SIGTERM,&signalled)==0 && signalled==std::set<pid_t>({daemon}),"signal scope included a tracked service");
        int status=0;check(waitpid(daemon,&status,WNOHANG)==0,"TERM-resistant daemon unexpectedly exited");
        check(waitpid(service,&status,WNOHANG)==0,"tracked service was signalled");
        check(pocketSignalAdopted32(services,SIGKILL)==0,"cannot kill adopted daemon");
        status=wait(daemon);check(WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL,"daemon was not reaped after escalation");
        check(waitpid(service,&status,WNOHANG)==0,"tracked service was killed during escalation");

        // A fork in a worker thread is not listed in the group leader's
        // task/children file. Kernel wait ownership must include every thread.
        worker=std::thread([&]{
            pid_t child=fork();
            if(!child){report(channel[1],3,getpid());for(;;)pause();}
            if(child<0)report(channel[1],3,-1);
            while(!release.load())usleep(1000);
        });
        Message threaded=receive(channel[0]);check(threaded.kind==3 && threaded.pid>1,"worker child failed");
        check(pocketOwnedChildren32(&children)==0 && children.count(threaded.pid),"worker-thread child omitted");
        check(pocketSignalAdopted32(services,SIGTERM)==0,"cannot signal worker-thread child");
        status=wait(threaded.pid);check(WIFSIGNALED(status) && WTERMSIG(status)==SIGTERM,"worker child was not reaped");
        release=true;worker.join();
        check(waitpid(service,&status,WNOHANG)==0,"tracked service did not survive adopted-child sweeps");
        check(kill(service,SIGTERM)==0,"cannot stop tracked service");
        status=wait(service);check(WIFSIGNALED(status) && WTERMSIG(status)==SIGTERM,"tracked service reap failed");
        check(pocketOwnedChildren32(&children)==0 && children.empty(),"owned children remain");
        puts("OWNED_CHILDREN_PASS subreaper=1 setsid_orphan_adopted=1 term_resistance=1 kill_escalation=1 tracked_service_preserved=1 worker_children=1 all_reaped=1");result=0;
    }catch(const std::exception& error){fprintf(stderr,"OWNED_CHILDREN_FAIL %s\n",error.what());}
    release=true;if(worker.joinable())worker.join();cleanup();
    for(int fd:channel)if(fd>=0)close(fd);
    return result;
}
