// SPDX-License-Identifier: GPL-3.0-only
// Real stock netd text frontend and responder registration callback.
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sstream>
#include <string>
namespace {
std::string property(const char* name) {char value[PROP_VALUE_MAX];__system_property_get(name,value);return value;}
bool sendCommand(int fd,unsigned sequence,const std::string& command) {
    std::string value=std::to_string(sequence)+" mdnssd "+command;value+='\0';
    const char* next=value.data();size_t left=value.size();
    while(left) {
        ssize_t count=send(fd,next,left,MSG_NOSIGNAL);if(count<0 && errno==EINTR)continue;
        if(count<=0)return false;next+=count;left-=count;
    }
    return true;
}
bool event(int fd,int& code,int& number,std::string& text) {
    text.clear();char byte;
    while(text.size()<4096) {
        ssize_t count=recv(fd,&byte,1,0);if(count<0 && errno==EINTR)continue;
        if(count!=1)return false;if(!byte)break;text+=byte;
    }
    std::istringstream input(text);if(!(input>>code>>number))return false;
    if(code>=400 && code!=500 && code!=501)
        fprintf(stderr,"API32_MDNS_REPLY code=%d request=%d\n",code,number);
    return true;
}
bool reply(int fd,int expectedCode,int sequence) {
    int code=0,number=0;std::string text;
    if(!event(fd,code,number,text))return false;
    if(code==expectedCode && number==sequence)return true;
    fprintf(stderr,"API32_MDNS_UNEXPECTED_REPLY expected=%d actual=%d sequence=%d\n",expectedCode,code,number);return false;
}
}
int main(int argc,char** argv) {
    setbuf(stdout,nullptr);
    if(argc!=2 || getuid()!=0)return 64;
    bool denied=!strcmp(argv[1],"denied");if(!denied && strcmp(argv[1],"check"))return 64;
    if(setgid(denied?39999:1000) || setuid(denied?39999:1000))return 64;
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return 1;
    timeval timeout{15,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,"/dev/socket/mdns");
    if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))){perror("API32_MDNS_CONNECT");return 2;}
    if(denied) {
        if(!sendCommand(fd,1,"start-service") || !reply(fd,400,1) || property("init.svc.mdnsd")=="running")return 3;
        puts("API32_MDNS_ROLE_DENIED_PASS uid=39999 start=denied");return 0;
    }
    if(!sendCommand(fd,1,"not-a-command") || !reply(fd,500,1) ||
       !sendCommand(fd,2,"stop-register 2147483647") || !reply(fd,501,2))return 4;
    if(!sendCommand(fd,3,"start-service") || !reply(fd,200,3) || property("init.svc.mdnsd")!="running")return 5;
    std::string name="PocketApi32-"+std::to_string(getpid());int request=getpid();
    std::string registration="register "+std::to_string(request)+" "+name+" _pocket-api32._tcp. 25451 A3Y9MQ==";
    if(!sendCommand(fd,4,registration))return 6;
    bool acknowledged=false,registered=false;
    for(unsigned n=0;n<4 && (!acknowledged || !registered);++n) {
        int code=0,number=0;std::string text;
        if(!event(fd,code,number,text)){perror("API32_MDNS_REGISTER_WAIT");return 7;}
        if(code==200 && number==4)acknowledged=true;
        else if(code==606 && number==request && text.find(name)!=std::string::npos)registered=true;
        else {fprintf(stderr,"API32_MDNS_REGISTER_FAILED code=%d number=%d\n",code,number);return 8;}
    }
    if(!acknowledged || !registered)return 9;
    if(!sendCommand(fd,5,"stop-register "+std::to_string(request)) || !reply(fd,200,5))return 10;
    // The stock monitor removes its registration asynchronously. Wait for
    // its actual stop result instead of racing a second deallocation.
    bool stopped=false;
    for(unsigned sequence=6;sequence<26 && !stopped;++sequence) {
        int code=0,number=0;std::string text;
        if(!sendCommand(fd,sequence,"stop-service") || !event(fd,code,number,text) || number!=static_cast<int>(sequence))return 11;
        if(code==200)stopped=property("init.svc.mdnsd")=="stopped";
        else if(code!=403)return 11;
        if(!stopped)usleep(50000);
    }
    if(!stopped)return 11;
    close(fd);
    puts("API32_MDNS_PASS stock_listener=12L stock_responder=1 registration_callback=real removal=1 daemon_start_stop=1 invalid_request=rejected");return 0;
}
