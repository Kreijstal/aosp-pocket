// SPDX-License-Identifier: GPL-3.0-only
#include "../../api32/external_app_dirs.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <map>
using pocket::storage32::AppDirectories;
static unsigned descriptors(){DIR* d=opendir("/proc/self/fd");assert(d);unsigned n=0;while(readdir(d))n++;closedir(d);return n;}
int main() {
    const unsigned before=descriptors();char temp[]="/tmp/pocket-appdirs-XXXXXX";assert(mkdtemp(temp));std::string root=temp,prefix="/storage/emulated/0/Android/";
    int fd=open(root.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(fd>=0);AppDirectories::Plan plan;
    for(const auto& path:{prefix+"data/pkg/../escape",prefix+"data/./files",prefix+"data/pkg//files",prefix+"data/pkg/./files",prefix+"data",prefix+"data/.hidden",prefix+"data/pkg-invalid",prefix+"datab/pkg",std::string("/storage/emulated/1/Android/data/pkg"),std::string("/data/media/0/Android/data/pkg"),prefix+"data/pkg/"+std::string(256,'x'),prefix+std::string("data/pkg\0/other",15)})assert(AppDirectories::parse(path,10001,&plan)==-EINVAL);
    for(int uid:{0,1000,9999,20000,99000,110001})assert(AppDirectories::parse(prefix+"data/pkg",uid,&plan)==-EINVAL);
    assert(!AppDirectories::parse(prefix+"data/pkg/files/",10001,&plan));
    std::map<ino_t,std::array<unsigned,3>> labels;unsigned calls=0;
    auto metadata=[&](int entry,uid_t uid,gid_t gid,mode_t mode){struct stat st{};assert(!fstat(entry,&st));labels[st.st_ino]={uid,gid,mode};calls++;return fchmod(entry,mode)?-errno:0;};
    assert(!AppDirectories::prepare(fd,plan,AppDirectories::Operation::FixExisting,metadata) && !calls && access((root+"/Android").c_str(),F_OK)<0);
    assert(!AppDirectories::prepare(fd,plan,AppDirectories::Operation::Setup,metadata));struct stat st{};
    auto target=root+"/Android/data/pkg/files";assert(!stat(target.c_str(),&st) && (st.st_mode&07777)==02770 && (labels.at(st.st_ino)==std::array<unsigned,3>{10001,1078,02770}));
    auto forged=plan;forged.names.back()="other/escape";assert(AppDirectories::prepare(fd,forged,AppDirectories::Operation::Setup,metadata)==-EINVAL);
    assert(!chmod(target.c_str(),0700));calls=0;assert(!AppDirectories::prepare(fd,plan,AppDirectories::Operation::EnsureExists,metadata) && !calls);assert(!stat(target.c_str(),&st) && (st.st_mode&07777)==0700);
    int file=open((target+"/canary").c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);assert(file>=0 && write(file,"data",4)==4 && !close(file));
    assert(!AppDirectories::prepare(fd,plan,AppDirectories::Operation::FixExisting,metadata));assert(!stat((target+"/canary").c_str(),&st) && (labels.at(st.st_ino)==std::array<unsigned,3>{10001,1078,02770}));
    assert(!unlink((target+"/canary").c_str()) && !rmdir(target.c_str()));
    assert(!mkdir((root+"/outside").c_str(),0700) && !symlink((root+"/outside").c_str(),target.c_str()));
    int result=AppDirectories::prepare(fd,plan,AppDirectories::Operation::Setup,metadata);assert(result==-ENOTDIR || result==-ELOOP);assert(!stat((root+"/outside").c_str(),&st) && (st.st_mode&07777)==0700);
    assert(!unlink(target.c_str()) && !rmdir((root+"/outside").c_str()));
    assert(!AppDirectories::parse(prefix+"obb/pkg",10001,&plan) && plan.gid==1079 && !AppDirectories::prepare(fd,plan,AppDirectories::Operation::Setup,metadata));
    assert(!AppDirectories::parse(prefix+"media/pkg",10001,&plan) && plan.gid==1023 && !AppDirectories::prepare(fd,plan,AppDirectories::Operation::Setup,metadata));
    for(const char* area:{"data","obb","media"})assert(!rmdir((root+"/Android/"+area+"/pkg").c_str()) && !rmdir((root+"/Android/"+area).c_str()));
    assert(!rmdir((root+"/Android").c_str()) && !close(fd) && !rmdir(root.c_str()) && descriptors()==before);
    puts("APP_DIRECTORIES_PASS canonical_user0_paths=1 app_uid_range=1 setup=1 ensure_existing_preserved=1 fix_missing_noop=1 fixup_entries=1 symlink_escape_denied=1 forged_plan_denied=1 fd_leaks=0 ownership_callback_model=1 native_permissions_claim=0");
}
