// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// PASS_THROUGH replaces kernel bind mounts with a process path view. Reverse
// its owned backing prefix too: MediaProvider canonicalizes its /storage
// paths when deriving the volume name. Exposing /data/media here classifies
// external rows as internal. Reuse the existing prefix space in the buffer;
// neither the buffer nor a socket pathname is grown.
static const char* external_lower_view(char* value,unsigned enabled) {
    if(!enabled || !prefix(value,pocket_loader_config.root,root_size))return 0;
    const char* lower=data_alias?"/system-data/media":"/data/media";
    size_t base=data_alias?sizeof("/system-data/media")-1:sizeof("/data/media")-1,head=root_size+base;
    if(head<17 || !prefix(value+root_size,lower,base))return 0;
    memcpy(value+head-17,"/storage/emulated",17);return value+head-17;
}
static const char* external_lower_visible_path(char* value) {
    return external_lower_view(value,external_storage_mode==3);
}
