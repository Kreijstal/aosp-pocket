// SPDX-License-Identifier: GPL-3.0-only
#include "../../api32/bootstrap_ready.h"
#include <cassert>
#include <cstdlib>
#include <string>
int main() {
    char directory[]="/tmp/pocket-bootstrap-ready-XXXXXX";assert(mkdtemp(directory));
    std::string staged=std::string(directory)+"/staged",ready=std::string(directory)+"/ready";
    auto write=[&](const std::string& bytes){int fd=open(staged.c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);assert(fd>=0 && ::write(fd,bytes.data(),bytes.size())==(ssize_t)bytes.size());close(fd);};
    auto publish=[&](const char* run){return pocketPublishBootReady32(run,staged.c_str(),ready.c_str());};
    assert(!publish("run-1"));
    write("");assert(!publish("run-1"));assert(unlink(staged.c_str())==0);
    for(const char* bytes:{"other\n","run-1","run-1\nextra"}){write(bytes);assert(!publish("run-1") && access(ready.c_str(),F_OK));assert(unlink(staged.c_str())==0);}
    write("run-1\n");assert(!publish(nullptr) && !publish("") && !publish("bad/name") && !publish(std::string(65,'a').c_str()));
    assert(publish("run-1") && access(staged.c_str(),F_OK));
    char value[8]{};int fd=open(ready.c_str(),O_RDONLY);assert(fd>=0 && read(fd,value,sizeof(value))==6 && !memcmp(value,"run-1\n",6));close(fd);
    assert(symlink(ready.c_str(),staged.c_str())==0 && !publish("run-1"));assert(unlink(staged.c_str())==0);
    assert(mkdir(staged.c_str(),0700)==0 && !publish("run-1"));assert(rmdir(staged.c_str())==0);
    assert(unlink(ready.c_str())==0 && rmdir(directory)==0);
    puts("BOOTSTRAP_READY_PASS missing=1 partial=1 wrong_nonce=1 extra_bytes=1 invalid_id=1 atomic_publish=1 symlink=denied directory=denied");
}
