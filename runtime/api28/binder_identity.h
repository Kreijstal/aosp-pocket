// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../loader/binder_identity.h"
#include <stdio.h>
#include <unistd.h>
// Rootless guest metadata, not a new kernel identity/security boundary.
static inline bool pocket_binder_identity(pid_t pid,uint64_t* value) {
 if(pid<=0)return false;
 char path[96],stat[4096];snprintf(path,sizeof(path),"/proc/%d/stat",pid);
 FILE*f=fopen(path,"r");if(!f)return false;
 size_t size=fread(stat,1,sizeof(stat),f);fclose(f);
 uint64_t start=pocket_process_start(stat,size);if(!start)return false;
 snprintf(path,sizeof(path),"/.pocket-identity/%d",pid);f=fopen(path,"rb");if(!f)return false;
 PocketBinderIdentity record={};bool ok=fread(&record,1,sizeof(record),f)==sizeof(record);fclose(f);
 if(!ok || record.magic!=POCKET_IDENTITY_MAGIC || record.pid!=(uint32_t)pid || record.start_ticks!=start)return false;
 *value=((uint64_t)record.uid<<32)|(uint32_t)pid;return true;
}
