// SPDX-License-Identifier: GPL-3.0-only
// Exercise the real classifier with CE-shaped empty lower directories.
#define main existing_external_symlink_main
#include "external_symlink_path_host.c"
#undef main

int main(void) {
    char root[]="/tmp/pocket-virtual-alias-XXXXXX";assert(mkdtemp(root));
    strcpy(pocket_loader_config.root,root);root_size=strlen(root);guest_uid[1]=10112;
    external_files_enabled=external_namespace_enabled=1;external_storage_mode=1;
    const char* dirs[]={"/system-data","/system-data/media","/system-data/media/0","/mnt","/mnt/runtime",
        "/mnt/runtime/default","/mnt/runtime/read","/mnt/runtime/write","/mnt/user","/mnt/user/0"};
    char physical[PATH_LIMIT],target[PATH_LIMIT],mapped[PATH_LIMIT],out[PATH_LIMIT],scratch[PATH_LIMIT];
    for(unsigned i=0;i<sizeof(dirs)/sizeof(*dirs);i++){path(physical,sizeof(physical),dirs[i]);assert(!mkdir(physical,0700));}
    const char* links[]={"/mnt/runtime/default/emulated","/mnt/runtime/read/emulated","/mnt/runtime/write/emulated","/mnt/user/0/primary"};
    for(unsigned i=0;i<4;i++){path(physical,sizeof(physical),links[i]);path(target,sizeof(target),i==3?"/system-data/media/0":"/system-data/media");assert(!symlink(target,physical));}
    const char* suffix="/0/Android/data/example/files/canary";
    const char* names[]={"/mnt/runtime/default/emulated/0/Android/data/example/files/canary",
        "/mnt/runtime/read/emulated/0/Android/data/example/files/canary",
        "/mnt/runtime/write/emulated/0/Android/data/example/files/canary",
        "/mnt/user/0/primary/Android/data/example/files/canary",
        "/data/media/0/Android/data/example/files/canary"};
    unsigned failures=0;
    for(unsigned i=0;i<5;i++) {
        assert(!map_path(names[i],mapped));
        int rc=external_file_path(AT_FDCWD,names[i],mapped,out,scratch);
        if(rc!=1 || strcmp(out,"/storage/emulated/0/Android/data/example/files/canary")) {
            printf("EXTERNAL_VIRTUAL_ALIAS_REJECT index=%u routed=%d\n",i,rc);failures++;
        }
    }
    path(target,sizeof(target),"/system-data/media");strcat(target,suffix);
    int rc=external_file_path(AT_FDCWD,target,target,out,scratch);
    if(rc!=1 || strcmp(out,"/storage/emulated/0/Android/data/example/files/canary")){printf("EXTERNAL_VIRTUAL_ALIAS_REJECT index=5 routed=%d\n",rc);failures++;}
    const char* unrelated="/mnt/runtime/default/emulated-other/0/a";
    assert(!map_path(unrelated,mapped));assert(!external_file_path(AT_FDCWD,unrelated,mapped,out,scratch));
    unrelated="/data/media-other/0/a";
    assert(!map_path(unrelated,mapped));assert(!external_file_path(AT_FDCWD,unrelated,mapped,out,scratch));
    char long_name[PATH_LIMIT];memset(long_name,'a',sizeof(long_name));memcpy(long_name,"/data/media/",12);long_name[PATH_LIMIT-1]=0;
    assert(external_file_path(AT_FDCWD,long_name,long_name,out,scratch)==-ENAMETOOLONG);
    // Backing access stays local for the privileged volume service/provider.
    guest_uid[1]=1000;assert(!map_path(names[4],mapped));assert(!external_file_path(AT_FDCWD,names[4],mapped,out,scratch));
    guest_uid[1]=10112;external_storage_mode=3;assert(!external_file_path(AT_FDCWD,names[0],mapped,out,scratch));
    external_storage_mode=1;external_namespace_enabled=0;
    assert(!map_path(names[0],mapped));assert(!external_file_path(AT_FDCWD,names[0],mapped,out,scratch));
    for(unsigned i=0;i<4;i++){path(physical,sizeof(physical),links[i]);assert(!unlink(physical));}
    for(unsigned i=sizeof(dirs)/sizeof(*dirs);i;i--){path(physical,sizeof(physical),dirs[i-1]);assert(!rmdir(physical));}
    assert(!rmdir(root));
    printf("EXTERNAL_VIRTUAL_ALIAS_%s cases=6 failures=%u privileged_lower_preserved=1 passthrough_preserved=1 disabled_namespace_preserved=1 component_boundaries=1 expansion_bound=1\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
