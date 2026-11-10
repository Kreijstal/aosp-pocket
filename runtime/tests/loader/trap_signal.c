// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// Deliver an application signal while the loader is blocked inside an
// intercepted write. The application handler itself needs intercepted I/O.
static volatile sig_atomic_t handled;
static const char* fixture="/fixture.txt";
static void application_signal(int number) {
    (void)number;
    char value[16]={0};
    int fd=open(fixture,O_RDONLY|O_CLOEXEC);
    if(fd>=0) {
        if(read(fd,value,15)==15 && !memcmp(value,"loader-fixture\n",15))handled=1;
        close(fd);
    }
}
#define CHECK(condition) do {if(!(condition)){perror(#condition);return 1;}}while(0)
int main(int argc,char** argv) {
    CHECK(argc==1 || argc==2);if(argc==2)fixture=argv[1];
    struct sigaction action={0};
    action.sa_handler=application_signal;action.sa_flags=SA_RESTART;
    sigemptyset(&action.sa_mask);CHECK(!sigaction(SIGUSR1,&action,NULL));
    int pipes[2];CHECK(!pipe2(pipes,O_CLOEXEC|O_NONBLOCK));
    char bytes[4096]={0};
    while(write(pipes[1],bytes,sizeof(bytes))==(ssize_t)sizeof(bytes)){}
    CHECK(errno==EAGAIN);CHECK(!fcntl(pipes[1],F_SETFL,0));
    pid_t owner=getpid(),child=fork();CHECK(child>=0);
    if(child==0) {
        usleep(100000);
        if(kill(owner,SIGUSR1))_exit(2);
        usleep(100000);
        _exit(read(pipes[0],bytes,sizeof(bytes))==(ssize_t)sizeof(bytes)?0:3);
    }
    CHECK(write(pipes[1],bytes,sizeof(bytes))==(ssize_t)sizeof(bytes));
    int status=0;CHECK(waitpid(child,&status,0)==child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0 && handled==1);
    close(pipes[0]);close(pipes[1]);
    puts("LOADER_SIGNAL_DURING_TRAP_PASS interrupted=blocking_write handler=filesystem_io");
    return 0;
}
