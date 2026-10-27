// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
#define POCKET_IDENTITY_MAGIC 0x504b4944u
struct PocketBinderIdentity { uint32_t magic,pid,uid,gid; uint64_t start_ticks; };
static inline uint64_t pocket_process_start(const char* text,size_t size) {
 size_t at=size;while(at && text[at-1]!=')')at--;if(!at)return 0;
 for(unsigned field=3;field<=22;field++) {
  while(at<size && text[at]==' ')at++;
  if(field==22){uint64_t n=0;unsigned digits=0;while(at<size && text[at]>='0' && text[at]<='9'){
   if(n>UINT64_MAX/10)return 0;n=n*10+(unsigned)(text[at++]-'0');digits++;
  }return digits?n:0;}
  while(at<size && text[at]!=' ')at++;
 }
 return 0;
}
