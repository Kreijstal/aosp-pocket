// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Public emulated storage is an outer MediaProvider namespace. Encryption
// owns only its backing tree; selecting CE must not bypass provider policy or
// expose the lower canonical pathname to Android's app-directory validation.
static int ce_external_defer(const char* path) {
    if(!external_namespace_enabled)return 0;
    const char* p=path;
    if(prefix(p,pocket_loader_config.root,root_size))p+=root_size;
    if(prefix(p,"/mnt/user/0/emulated",20))return 1; // Forced-FUSE even for MediaProvider.
    if(external_storage_mode==3)return 0;
    const char* public[]={"/storage/emulated","/sdcard","/storage/self/primary","/mnt/user",
        "/mnt/runtime/default/emulated","/mnt/runtime/read/emulated","/mnt/runtime/write/emulated"};
    for(unsigned i=0;i<sizeof(public)/sizeof(*public);i++) {
        if(prefix(p,public[i],length(public[i],PATH_LIMIT)))return 1;
    }
    // Privileged runtime code prepares the lower tree before a session
    // exists. Ordinary apps must still reach it through MediaProvider.
    return guest_uid[1] && guest_uid[1]!=1000
        && (prefix(p,"/data/media",11) || prefix(p,"/system-data/media",18));
}
static int ce_user_path(const char* path,const char* base) {
    size_t n=length(base,PATH_LIMIT);return prefix(path,base,n) && path[n]=='/' && path[n+1]>='0' && path[n+1]<='9';
}
static int ce_candidate(const char* path) {
    if(ce_external_defer(path))return 0;
    const char* p=path;
    if(prefix(p,pocket_loader_config.root,root_size)) {
        p+=root_size;if(prefix(p,"/.pocket-ce-dirs",16))return 1;
        if(prefix(p,"/system-data",12)){p+=12;return ce_user_path(p,"/user")||prefix(p,"/data",5)||ce_user_path(p,"/system_ce")||ce_user_path(p,"/misc_ce")||ce_user_path(p,"/media")||(ce_vendor_enabled&&ce_user_path(p,"/vendor_ce"));}
    }
    if(ce_user_path(p,"/data/user") || ce_user_path(p,"/data/system_ce") || ce_user_path(p,"/data/misc_ce") || ce_user_path(p,"/data/media") || ce_user_path(p,"/storage/emulated"))return 1;
    if(ce_vendor_enabled && ce_user_path(p,"/data/vendor_ce"))return 1;
    if(ce_user_path(p,"/mnt/runtime/default/emulated") || ce_user_path(p,"/mnt/runtime/read/emulated") || ce_user_path(p,"/mnt/runtime/write/emulated"))return 1;
    if(ce_user_path(p,"/mnt/user")){const char* tail=p+10;while(*tail>='0'&&*tail<='9')tail++;if(prefix(tail,"/primary",8))return 1;}
    const char* paths[]={"/data/user/0","/data/data","/data/system_ce/0","/data/misc_ce/0","/data/media/0","/sdcard","/storage/emulated/0","/storage/self/primary","/mnt/user/0/primary","/mnt/runtime/default/emulated/0","/mnt/runtime/read/emulated/0","/mnt/runtime/write/emulated/0"};
    for(unsigned i=0;i<sizeof(paths)/sizeof(*paths);i++) {
        if(prefix(p,paths[i],length(paths[i],PATH_LIMIT)))return 1;
    }
    return 0;
}
// A CE directory carrier reports a guest canonical path, rather than a
// physical owned-root path. Apply the same PASS_THROUGH reversal used by the
// plaintext loader, including short readlink buffers and length bounds.
static long ce_media_visible(char* value,unsigned* size,size_t capacity) {
    size_t base=0,n=*size;if(n>capacity || n>=PATH_LIMIT)return -ENAMETOOLONG;
    unsigned terminated=n && value[n-1]==0;if(terminated)n--;
    if(n>=11 && equal(value,"/data/media",11) && (n==11 || value[11]=='/'))base=11;
    else if(n>=18 && equal(value,"/system-data/media",18) && (n==18 || value[18]=='/'))base=18;
    if(!base)return 0;
    size_t tail=n-base,total=17+tail;
    if(total>=capacity || total>=PATH_LIMIT)return -ENAMETOOLONG;
    if(base<17)for(size_t i=tail;i;i--)value[17+i-1]=value[base+i-1];
    else for(size_t i=0;i<tail;i++)value[17+i]=value[base+i];
    memcpy(value,"/storage/emulated",17);value[total]=0;*size=(unsigned)total+terminated;return 0;
}
static long ce_external_visible(char* value,unsigned* size,size_t capacity) {
    if(!external_namespace_enabled || external_storage_mode!=3)return 0;
    return ce_media_visible(value,size,capacity);
}
static long ce_external_cwd(char* value,unsigned* size,size_t capacity) {
    // Kernel cwd can be a CE directory anchor received from the provider.
    // Match the ordinary loader's selected-mount view, preserving the NUL
    // counted by getcwd while readlink continues to exclude its terminator.
    if(!external_namespace_enabled)return 0;
    return ce_media_visible(value,size,capacity);
}
