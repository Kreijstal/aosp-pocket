// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define PATH_LIMIT 4096
static struct {char root[1024];} pocket_loader_config={"/owned/root"};
static size_t root_size=11;
static unsigned guest_uid[3],external_storage_mode;
static int ce_vendor_enabled=1,external_namespace_enabled;
static size_t length(const char* value,size_t limit){return strnlen(value,limit);}
static int equal(const char* a,const char* b,size_t n){return !memcmp(a,b,n);}
static int prefix(const char* a,const char* b,size_t n){return !strncmp(a,b,n) && (!a[n] || a[n]=='/');}
#include "../loader/ce_path_policy.h"

int main(void) {
    const char* public[]={"/storage/emulated/0/Android/data/example/files","/sdcard/a","/storage/self/primary/a",
        "/mnt/user/0/primary/a","/mnt/runtime/default/emulated/0/a","/mnt/runtime/read/emulated/0/a","/mnt/runtime/write/emulated/0/a"};
    const char* lower[]={"/data/media/0/a","/owned/root/system-data/media/0/a","/owned/root/data/media/0/a"};
    const char* private[]={"/data/user/0/example/a","/data/data/example/a","/data/system_ce/0/a","/data/misc_ce/0/a",
        "/data/vendor_ce/0/a","/owned/root/.pocket-ce-dirs/13","/owned/root/system-data/user/0/example/a"};
    unsigned checks=0;
    for(unsigned enabled=0;enabled<2;enabled++)for(unsigned mode=0;mode<5;mode++)for(unsigned actor=0;actor<3;actor++) {
        const unsigned ids[]={0,1000,10112};external_namespace_enabled=(int)enabled;external_storage_mode=mode;guest_uid[1]=ids[actor];
        for(unsigned i=0;i<sizeof(public)/sizeof(*public);i++) {
            assert(ce_candidate(public[i])==(int)(!enabled || mode==3));checks++;
        }
        for(unsigned i=0;i<sizeof(lower)/sizeof(*lower);i++) {
            assert(ce_candidate(lower[i])==(int)(!enabled || mode==3 || actor<2));checks++;
        }
        for(unsigned i=0;i<sizeof(private)/sizeof(*private);i++){assert(ce_candidate(private[i]));checks++;}
        assert(!ce_candidate("/storage/emulated-other/0/a") && !ce_candidate("/data/media-other/0/a"));checks+=2;
        char path[PATH_LIMIT];strcpy(path,"/data/media/0/Android/data/example/files");unsigned n=(unsigned)strlen(path);
        assert(!ce_external_visible(path,&n,sizeof(path)));
        assert(!strcmp(path,enabled && mode==3?"/storage/emulated/0/Android/data/example/files":"/data/media/0/Android/data/example/files"));
        assert(n==strlen(path));checks++;
        const char* cwds[]={"/data/media/0/a","/system-data/media/0/a","/data/user/0/example/a","/data/media-other/0/a"};
        for(unsigned c=0;c<sizeof(cwds)/sizeof(*cwds);c++) {
            strcpy(path,cwds[c]);n=(unsigned)strlen(path)+1;
            assert(!ce_external_cwd(path,&n,sizeof(path)));
            assert(!strcmp(path,enabled && c<2?"/storage/emulated/0/a":cwds[c]));
            assert(n==strlen(path)+1 && path[n-1]==0);checks++;
        }
    }
    external_namespace_enabled=1;external_storage_mode=3;
    char path[PATH_LIMIT];strcpy(path,"/system-data/media/0/a");unsigned n=(unsigned)strlen(path);
    assert(!ce_external_visible(path,&n,sizeof(path)) && !strcmp(path,"/storage/emulated/0/a") && n==21);checks++;
    strcpy(path,"/data/media-other/0/a");n=(unsigned)strlen(path);assert(!ce_external_visible(path,&n,sizeof(path)) && !strcmp(path,"/data/media-other/0/a"));checks++;
    strcpy(path,"/data/media/0/a");n=(unsigned)strlen(path);assert(ce_external_visible(path,&n,10)==-ENAMETOOLONG && !strcmp(path,"/data/media/0/a"));checks++;
    memset(path,'a',sizeof(path));memcpy(path,"/data/media/",12);path[PATH_LIMIT-2]=0;n=PATH_LIMIT-2;
    assert(ce_external_visible(path,&n,sizeof(path))==-ENAMETOOLONG && n==PATH_LIMIT-2);checks++;
    assert(!ce_candidate("/mnt/user/0/emulated/0/a"));checks++;
    ce_vendor_enabled=0;assert(!ce_candidate("/data/vendor_ce/0/a"));checks++;
    strcpy(path,"/data/media/0/a");n=(unsigned)strlen(path)+1;
    assert(ce_external_cwd(path,&n,strlen("/storage/emulated/0/a"))==-ENAMETOOLONG);
    assert(!strcmp(path,"/data/media/0/a") && n==strlen(path)+1);checks++;
    assert(!ce_external_cwd(path,&n,strlen("/storage/emulated/0/a")+1));
    assert(!strcmp(path,"/storage/emulated/0/a") && n==strlen(path)+1);checks++;
    memset(path,'a',sizeof(path));memcpy(path,"/data/media/",12);n=PATH_LIMIT;
    assert(ce_external_cwd(path,&n,sizeof(path))==-ENAMETOOLONG && n==PATH_LIMIT);checks++;
    printf("CE_EXTERNAL_ROUTE_PASS checks=%u outer_provider_before_ce=1 privileged_lower_preserved=1 passthrough_backing_encrypted=1 provider_canonical_public=1 disabled_profile_unchanged=1 bounds=1\n",checks);
}
