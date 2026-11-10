// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string>
#include "storage_modules32.h"

// Host files stay owned by the app. UID/GID here are guest loader metadata.
inline int storageDirectory32(int parent,const char* name,mode_t mode,uid_t uid,gid_t gid) {
    if(mkdirat(parent,name,mode) && errno!=EEXIST)return -1;
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
    struct stat st;
    if(fchown(fd,uid,gid) || fchmod(fd,mode) || fstat(fd,&st) || !S_ISDIR(st.st_mode) ||
       st.st_uid!=uid || st.st_gid!=gid || (st.st_mode&07777)!=mode) {close(fd);return -1;}
    return fd;
}
inline bool storageChild32(int parent,const char* name,mode_t mode,uid_t uid,gid_t gid) {
    int fd=storageDirectory32(parent,name,mode,uid,gid);if(fd<0)return false;close(fd);return true;
}
inline int storageUserRoot32(int data,const char* name,mode_t topMode,mode_t mode,uid_t uid,gid_t gid,
        uid_t topUid=1000,gid_t topGid=1000) {
    int top=storageDirectory32(data,name,topMode,topUid,topGid);if(top<0)return -1;
    int user=storageDirectory32(top,"0",mode,uid,gid);close(top);return user;
}
inline bool storageApex32(int misc) {
    int apex=storageDirectory32(misc,"apexdata",0711,0,0);if(apex<0)return false;
    bool result=true;
    for(const char* name:modules32)if(!storageChild32(apex,name,0771,0,1000)){result=false;break;}
    close(apex);return result;
}
inline bool storageMisc32(int misc) {
    bool ready=true;
    for(const char* name:{"vold","storaged","rollback","apexrollback"})
        if(!storageChild32(misc,name,0700,0,0)){ready=false;break;}
    return ready && storageApex32(misc);
}
inline bool prepareStorageUser032(int flags) {
    // The authenticated service permits CE preparation only after the
    // encrypted namespace unlocks. Its five roots are intercepted by the loader.
    int data=open("/data",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(data<0)return false;
    bool ready=true;
    if(flags&1) {
        int system=storageUserRoot32(data,"system_de",0770,0770,1000,1000);
        int misc=storageUserRoot32(data,"misc_de",01771,01771,1000,9998,1000,9998);
        int vendor=storageUserRoot32(data,"vendor_de",0771,0771,0,0,0,0);
        int user=storageUserRoot32(data,"user_de",0711,0771,1000,1000);
        ready=system>=0 && misc>=0 && vendor>=0 && user>=0;
        if(ready)ready=storageMisc32(misc) && storageChild32(vendor,"fpdata",0700,1000,1000) &&
            storageChild32(vendor,"facedata",0700,1000,1000);
        for(int fd:{system,misc,vendor,user})if(fd>=0)close(fd);
        int legacy=storageDirectory32(data,"system",0775,1000,1000);
        int users=legacy<0?-1:storageDirectory32(legacy,"users",0775,1000,1000);
        if(legacy>=0)close(legacy);
        if(users<0)ready=false;
        else {ready=ready && storageChild32(users,"0",0700,1000,1000);close(users);}
        int miscTop=storageDirectory32(data,"misc",01771,1000,9998);
        int miscUsers=miscTop<0?-1:storageDirectory32(miscTop,"user",0771,0,0);
        if(miscTop>=0)close(miscTop);
        if(miscUsers<0)ready=false;
        else {ready=ready && storageChild32(miscUsers,"0",0750,1000,9997);close(miscUsers);}
        int profiles=-1;
        miscTop=storageDirectory32(data,"misc",01771,1000,9998);
        if(miscTop>=0)profiles=storageDirectory32(miscTop,"profiles",0771,1000,1000);
        if(miscTop>=0)close(miscTop);
        int current=profiles<0?-1:storageDirectory32(profiles,"cur",0771,1000,1000);
        if(profiles>=0)close(profiles);
        if(current<0)ready=false;
        else {ready=ready && storageChild32(current,"0",0771,1000,1000);close(current);}
    }
    if(ready && (flags&2)) {
        int system=storageUserRoot32(data,"system_ce",0770,0770,1000,1000);
        int misc=storageUserRoot32(data,"misc_ce",01771,01771,1000,9998,1000,9998);
        int vendor=storageUserRoot32(data,"vendor_ce",0771,0771,0,0,0,0);
        int user=storageUserRoot32(data,"user",0711,0771,1000,1000);
        // Media data is prepared but no emulated/FUSE volume is advertised.
        int mediaTop=storageDirectory32(data,"media",0770,1023,1023);
        int media=mediaTop<0?-1:storageDirectory32(mediaTop,"0",02770,1023,1023);
        if(mediaTop>=0)close(mediaTop);
        ready=system>=0 && misc>=0 && vendor>=0 && user>=0 && media>=0;
        if(ready)ready=storageMisc32(misc) && storageChild32(system,"backup",0700,1000,1000) &&
            storageChild32(system,"backup_stage",0700,1000,1000) && storageChild32(vendor,"facedata",0700,1000,1000);
        for(int fd:{system,misc,vendor,user,media})if(fd>=0)close(fd);
    }
    close(data);return ready;
}
