// SPDX-License-Identifier: GPL-3.0-only
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <initializer_list>
#include <vector>

static int waitChild(pid_t child) {
    for(unsigned i=0;i<400;++i) {
        int status=0;pid_t result=waitpid(child,&status,WNOHANG);
        if(result==child)return WIFEXITED(status)?WEXITSTATUS(status):-1;
        if(result<0)return -1;
        usleep(25000);
    }
    kill(child,SIGKILL);waitpid(child,nullptr,0);return -1;
}
static int command(uid_t uid,std::initializer_list<const char*> arguments) {
    pid_t child=fork();if(child<0)return -1;
    if(!child) {
        if(setgid(uid) || setuid(uid))_exit(125);
        std::vector<char*> argv;
        for(const char* arg:arguments)argv.push_back(const_cast<char*>(arg));
        argv.push_back(nullptr);execv(argv[0],argv.data());_exit(126);
    }
    return waitChild(child);
}
static int countFds(pid_t pid) {
    char path[80];snprintf(path,sizeof(path),"/proc/%d/fd",pid);
    DIR* directory=opendir(path);if(!directory)return -1;
    int count=0;while(auto* entry=readdir(directory))if(entry->d_name[0]!='.')++count;
    closedir(directory);return count;
}
static bool unregisteredRejected() {
    pid_t child=fork();if(child<0)return false;
    if(!child) {
        // Remove only this disposable child's identity. Its genuine kernel UID
        // still matches the app, which alone must not authenticate it to traced.
        if(setgid(0) || setuid(0))_exit(1);
        char path[80];snprintf(path,sizeof(path),"/.pocket-identity/%d",getpid());
        if(unlink(path))_exit(1);
        int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)_exit(2);
        sockaddr_un address={};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/traced_consumer");
        timeval timeout={2,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)))_exit(3);
        char byte;ssize_t got=read(fd,&byte,1);int error=errno;close(fd);
        _exit(got==0 || (got<0 && error==ECONNRESET)?0:4);
    }
    return waitChild(child)==0;
}
static unsigned tracePackets(const char* path,off_t size) {
    if(size<=0 || size>1024*1024)return 0;
    FILE* input=fopen(path,"rb");if(!input)return 0;
    unsigned packets=0;bool valid=true;
    while(ftell(input)<size) {
        // A Trace is a sequence of length-delimited field-1 TracePackets.
        if(fgetc(input)!=0x0a){valid=false;break;}
        unsigned length=0,shift=0;bool complete=false;
        for(unsigned i=0;i<5;++i) {
            int byte=fgetc(input);
            if(byte<0 || (i==4 && byte>15))break;
            length|=static_cast<unsigned>(byte&127)<<shift;shift+=7;
            if(!(byte&128)){complete=true;break;}
        }
        long position=ftell(input);
        if(!complete || position<0 || length>static_cast<unsigned long long>(size-position) ||
                fseek(input,length,SEEK_CUR)){valid=false;break;}
        ++packets;
    }
    valid=valid && !ferror(input) && ftell(input)==size;fclose(input);
    return valid?packets:0;
}
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);if(getuid()!=0 || argc!=2)return 64;
    pid_t server=atoi(argv[1]);if(server<=1 || server==getpid())return 64;
    char key[80],detach[100],check[100],attach[100],config[100],output[100];
    snprintf(key,sizeof(key),"pocket-probe-%d-%lld",getpid(),static_cast<long long>(time(nullptr)));
    snprintf(detach,sizeof(detach),"--detach=%s",key);
    snprintf(check,sizeof(check),"--is_detached=%s",key);
    snprintf(attach,sizeof(attach),"--attach=%s",key);
    snprintf(config,sizeof(config),"/tmp/%s.cfg",key);snprintf(output,sizeof(output),"/tmp/%s.trace",key);
    FILE* stream=fopen(config,"wx");if(!stream)return 1;
    const char* text="buffers { size_kb: 64 }\nduration_ms: 20000\nwrite_into_file: true\n";
    bool written=fwrite(text,1,strlen(text),stream)==strlen(text);if(fclose(stream) || !written)return 2;
    if(chmod(config,0644))return 3;
    // The actual service must answer a missing session with the documented 2.
    int missing=-1;
    for(unsigned i=0;i<3;++i) {
        missing=command(1000,{"/system/bin/perfetto",check});if(missing==2)break;
        usleep(20000);
    }
    if(missing!=2)return 4;
    if(command(1000,{"/system/bin/perfetto","--txt","-c",config,"-o",output,detach}))return 5;
    if(command(1000,{"/system/bin/perfetto",check}))return 6;
    if(command(39999,{"/system/bin/perfetto",check})!=2)return 7;
    if(command(1000,{"/system/bin/perfetto",attach,"--stop"}))return 8;
    if(command(1000,{"/system/bin/perfetto",check})!=2)return 9;
    struct stat trace={};if(stat(output,&trace) || !S_ISREG(trace.st_mode) || trace.st_size<=0)return 10;
    unsigned packets=tracePackets(output,trace.st_size);if(!packets)return 15;
    if(!unregisteredRejected())return 11;
    int before=countFds(server);if(before<0)return 12;
    for(unsigned i=0;i<24;++i)if(!unregisteredRejected())return 13;
    int after=-1;
    for(unsigned i=0;i<100;++i){after=countFds(server);if(after==before)break;usleep(20000);}
    if(after!=before || command(1000,{"/system/bin/perfetto",check})!=2)return 14;
    printf("API32_PERFETTO_UNREGISTERED_DENIED_PASS requests=24 same_kernel_uid=1 daemon_fd_before=%d daemon_fd_after=%d service_alive=1\n",before,after);
    printf("API32_PERFETTO_SESSION_PASS owner_uid=1000 other_uid=39999 other_attach=denied stopped=1 trace_bytes=%lld framed_packets=%u stock_client=1 stock_service=1\n",static_cast<long long>(trace.st_size),packets);
    unlink(config);unlink(output);return 0;
}
