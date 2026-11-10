// SPDX-License-Identifier: GPL-3.0-only
#pragma once
static long map_path_policy(const char* path,char out[PATH_LIMIT],unsigned parentProbe) {
    if(denylist_view && denylist_hidden(path))return -ENOENT;
    if(equal(path,"/dev/hwbinder",14))path="/dev/binder";
    size_t n=length(path,PATH_LIMIT);
    if(n==PATH_LIMIT)return -ENAMETOOLONG;
    if(path[0]!='/' || prefix(path,pocket_loader_config.root,root_size) ||
       prefix(path,"/proc",5) || prefix(path,"/sys",4) ||
       (prefix(path,"/dev",4) && !prefix(path,"/dev/socket",11) &&
        !prefix(path,"/dev/__properties__",19) && !prefix(path,"/dev/input",10) &&
        // Recovery/Magisk installer scratch space is guest-private, not a device.
        !prefix(path,"/dev/tmp",8))) {
        memcpy(out,path,n+1);return 0;
    }
    // Lexical escape rejection only. Symlink containment is not provided.
    if(!parentProbe)for(size_t i=0;i<n;i++)if(path[i]=='/' && path[i+1]=='.' && path[i+2]=='.' &&
        (!path[i+3] || path[i+3]=='/'))return -EPERM;
    // MediaProvider's explicit PASS_THROUGH view reaches the backing media
    // tree. Its /mnt/user/<user>/emulated paths remain forced FUSE requests;
    // mapping those to the backing tree would bypass redaction/access checks.
    if(external_storage_mode==3 && prefix(path,"/storage/emulated",17)) {
        const char* backing=data_alias?"/system-data/media":"/data/media";
        size_t base=length(backing,32);
        if(root_size+base+n-17>=PATH_LIMIT)return -ENAMETOOLONG;
        memcpy(out,pocket_loader_config.root,root_size);memcpy(out+root_size,backing,base);
        memcpy(out+root_size+base,path+17,n-17+1);return 0;
    }
    if(data_alias && prefix(path,"/data",5)) {
        if(root_size+12+n-5>=PATH_LIMIT)return -ENAMETOOLONG;
        memcpy(out,pocket_loader_config.root,root_size);memcpy(out+root_size,"/system-data",12);
        memcpy(out+root_size+12,path+5,n-5+1);return 0;
    }
    // Magic Mount compatibility: Magisk publishes a fully merged private tree.
    // It supplies enumeration as well as open/stat/exec/dlopen semantics.
    if(module_view && !denylist_view && (prefix(path,"/system",7) || prefix(path,"/vendor",7))) {
        if(root_size+20+n>=PATH_LIMIT)return -ENAMETOOLONG;
        memcpy(out,pocket_loader_config.root,root_size);memcpy(out+root_size,"/.pocket-module-view",20);
        memcpy(out+root_size+20,path,n+1);return 0;
    }
    if(root_size+n>=PATH_LIMIT)return -ENAMETOOLONG;
    memcpy(out,pocket_loader_config.root,root_size);memcpy(out+root_size,path,n+1);return 0;
}
static long map_path(const char* path,char out[PATH_LIMIT]) {
    return map_path_policy(path,out,0);
}
// A parent-containing guest path may be prepared only for external routing.
// The caller MUST reject a successful deferred probe if no namespace broker
// handles it; it must never fall through to a raw host path operation.
static long map_path_parent_probe(const char* path,char out[PATH_LIMIT],unsigned enabled,unsigned* pending) {
    long result=map_path(path,out);
    if(result==-EPERM && enabled && !denylist_view) {
        result=map_path_policy(path,out,1);
        if(result>=0)*pending=1;
    }
    return result;
}
