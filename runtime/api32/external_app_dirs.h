// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace pocket::storage32 {
// Rootless counterpart of vold's PrepareAppDirFromRoot. The caller is the
// authenticated UID0 storage service; root is its owned /data/media/0 FD.
// UID/GID are loader metadata, not host chown, quota, or kernel ACL claims.
class AppDirectories final {
public:
    enum class Operation {Setup,FixExisting,EnsureExists};
    struct Plan {std::string area,package;std::vector<std::string> names;uid_t uid=0;gid_t gid=0;};
    using Metadata=std::function<int(int,uid_t,gid_t,mode_t)>;
    static int parse(const std::string& path,int32_t uid,Plan* result) {
        const std::string prefix="/storage/emulated/0/Android/";
        if(!result || uid<10000 || uid>19999 || path.size()>4095 || path.compare(0,prefix.size(),prefix) || path.find('\0')!=std::string::npos)return -EINVAL;
        Plan plan;plan.uid=uid;
        for(size_t at=prefix.size();at<path.size();) {
            size_t end=path.find('/',at);if(end==std::string::npos)end=path.size();auto name=path.substr(at,end-at);at=end+1;
            if(name.empty() || name=="." || name==".." || name.size()>255)return -EINVAL;
            plan.names.push_back(std::move(name));
        }
        if(plan.names.size()<2)return -EINVAL;
        plan.area=plan.names[0];plan.package=plan.names[1];
        if(plan.area=="data")plan.gid=1078;
        else if(plan.area=="obb")plan.gid=1079;
        else if(plan.area=="media")plan.gid=1023;
        else return -EINVAL;
        for(char c:plan.package)if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='.'))return -EINVAL;
        if(plan.package.front()=='.')return -EINVAL;
        *result=std::move(plan);return 0;
    }
    static int metadata(int fd,uid_t uid,gid_t gid,mode_t mode) {
        if(fchown(fd,uid,gid) || fchmod(fd,mode))return -errno;
        struct stat state{};if(fstat(fd,&state))return -errno;
        return state.st_uid==uid && state.st_gid==gid && (state.st_mode&07777)==mode?0:-EIO;
    }
private:
    struct Descriptor {int fd=-1;explicit Descriptor(int value=-1):fd(value){}~Descriptor(){if(fd>=0)close(fd);}void reset(int next){if(fd>=0)close(fd);fd=next;}Descriptor(const Descriptor&)=delete;};
    static int directory(int parent,const std::string& name,bool create) {
        if(create && mkdirat(parent,name.c_str(),0700) && errno!=EEXIST)return -errno;
        int fd=openat(parent,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);return fd<0?-errno:fd;
    }
    static int existing(int root,const Plan& plan) {
        Descriptor current(fcntl(root,F_DUPFD_CLOEXEC,0));if(current.fd<0)return -errno;
        for(const auto& name:plan.names) {
            int next=directory(current.fd,name,false);if(next<0)return next;current.reset(next);
        }
        return 0;
    }
    static int fixEntries(int fd,uid_t uid,gid_t gid,const Metadata& owner) {
        int copy=fcntl(fd,F_DUPFD_CLOEXEC,0);if(copy<0)return -errno;
        DIR* list=fdopendir(copy);if(!list){int error=-errno;close(copy);return error;}
        int result=0;unsigned count=0;
        for(;;) {
            errno=0;dirent* entry=readdir(list);if(!entry){if(errno)result=-errno;break;}
            if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
            if(++count>65536){result=-E2BIG;break;}
            Descriptor child(openat(fd,entry->d_name,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));
            if(child.fd<0){result=-errno;break;}
            struct stat state{};if(fstat(child.fd,&state)){result=-errno;break;}
            if(!S_ISREG(state.st_mode) && !S_ISDIR(state.st_mode)){result=-EOPNOTSUPP;break;}
            result=owner(child.fd,uid,gid,02770);if(result<0)break;
        }
        closedir(list);return result;
    }
public:
    static int prepare(int root,const Plan& plan,Operation operation,const Metadata& owner=metadata) {
        // Validate the public Plan again before any filesystem mutation.
        Plan checked;std::string path="/storage/emulated/0/Android";for(const auto& name:plan.names)path+="/"+name;
        int rc=parse(path,plan.uid,&checked);if(rc<0 || checked.names!=plan.names || checked.area!=plan.area || checked.package!=plan.package || checked.gid!=plan.gid)return -EINVAL;
        if(root<0 || !owner)return -EINVAL;
        Descriptor android(directory(root,"Android",false));
        int found=android.fd<0?android.fd:existing(android.fd,plan);
        if(found<0 && found!=-ENOENT)return found;
        if(operation==Operation::FixExisting && found==-ENOENT)return 0;
        if(operation==Operation::EnsureExists && !found)return 0;
        if(android.fd<0){int next=directory(root,"Android",true);if(next<0)return next;android.reset(next);}
        rc=owner(android.fd,1023,1023,02771);if(rc<0)return rc;
        Descriptor area;
        for(const char* name:{"data","obb","media"}) {
            Descriptor next(directory(android.fd,name,true));if(next.fd<0)return next.fd;
            gid_t gid=!strcmp(name,"data")?1078:!strcmp(name,"obb")?1079:1023;
            rc=owner(next.fd,1023,gid,02771);if(rc<0)return rc;
            if(plan.area==name){int fd=fcntl(next.fd,F_DUPFD_CLOEXEC,0);if(fd<0)return -errno;area.reset(fd);}
        }
        for(size_t at=1;at<plan.names.size();at++) {
            int fd=directory(area.fd,plan.names[at],operation!=Operation::FixExisting);if(fd<0)return fd;area.reset(fd);
            rc=owner(area.fd,plan.uid,plan.gid,02770);if(rc<0)return rc;
            if(operation==Operation::FixExisting){rc=fixEntries(area.fd,plan.uid,plan.gid,owner);if(rc<0)return rc;}
        }
        return 0;
    }
};
}
