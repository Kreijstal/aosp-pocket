// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct {char root[128];} pocket_loader_config={"/owned/root"};
static size_t root_size=11;
static unsigned external_storage_mode;
static int data_alias;
static int prefix(const char* a,const char* b,size_t size){return !strncmp(a,b,size) && (!a[size] || a[size]=='/');}
#include "../loader/external_visible_path.h"
int main(void) {
    char value[4096];external_storage_mode=3;
    for(int alias=0;alias<2;alias++) {
        data_alias=alias;const char* lower=alias?"/system-data/media":"/data/media";
        snprintf(value,sizeof(value),"%s%s/0/Pictures/a.png",pocket_loader_config.root,lower);
        size_t before=strlen(value);const char* result=external_lower_visible_path(value);assert(result && !strcmp(result,"/storage/emulated/0/Pictures/a.png") && strlen(value)==before);
        snprintf(value,sizeof(value),"%s%s",pocket_loader_config.root,lower);result=external_lower_visible_path(value);assert(result && !strcmp(result,"/storage/emulated"));
        snprintf(value,sizeof(value),"%s%s-other/file",pocket_loader_config.root,lower);assert(!external_lower_visible_path(value));
        snprintf(value,sizeof(value),"%s%s/0/deleted (deleted)",pocket_loader_config.root,lower);result=external_lower_visible_path(value);assert(result && !strcmp(result,"/storage/emulated/0/deleted (deleted)"));
    }
    strcpy(value,"/outside/root/system-data/media/0/file");assert(!external_lower_visible_path(value));
    for(unsigned mode=0;mode<5;mode++)if(mode!=3){external_storage_mode=mode;strcpy(value,"/owned/root/system-data/media/0/file");assert(!external_lower_visible_path(value));}
    for(int alias=0;alias<2;alias++) {
        data_alias=alias;snprintf(value,sizeof(value),"%s%s/0/moved",pocket_loader_config.root,alias?"/system-data/media":"/data/media");
        assert(!strcmp(external_lower_view(value,1),"/storage/emulated/0/moved"));
    }
    strcpy(value,"/owned/root/data/media-other/0/file");assert(!external_lower_view(value,1));
    puts("EXTERNAL_VISIBLE_PATH_PASS passthrough_reverse_prefix=1 both_backing_layouts=1 component_boundary=1 other_views_preserved=1 no_buffer_growth=1");
}
