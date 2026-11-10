// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
struct PocketBinderToken { size_t string_offset;uint32_t policy;uint64_t identity;unsigned format;int32_t work_source; };
static inline uint32_t pocket_token_word(const unsigned char* data,size_t at) {uint32_t n;memcpy(&n,data+at,4);return n;}
static inline int pocket_token_string(const unsigned char*data,size_t size,size_t at) {
 if(at>size || size-at<4)return 0;uint32_t n=pocket_token_word(data,at);
 if(!n || n>1024 || (uint64_t)(n+1)*2>size-at-4)return 0;
 // Interface descriptors are bounded ASCII; avoid confusing flags with length.
 for(unsigned i=0;i<n;i++)if(!data[at+4+i*2] || data[at+5+i*2] || data[at+4+i*2]<32)return 0;
 return data[at+4+n*2]==0 && data[at+5+n*2]==0;
}
static inline int pocket_binder_token(const void* bytes,size_t size,struct PocketBinderToken*out) {
 const unsigned char*d=(const unsigned char*)bytes;if(size<8)return 0;memset(out,0,sizeof(*out));
 out->work_source=-1;
 out->policy=pocket_token_word(d,0);
 if(size>=16 && pocket_token_word(d,8)==0x53595354 && pocket_token_string(d,size,12)){
  out->string_offset=12;out->format=2;out->work_source=(int32_t)pocket_token_word(d,4);return 1;
 }
 if(size>=24 && pocket_token_word(d,16)==0x53595354 && pocket_token_string(d,size,20)){
  memcpy(&out->identity,d,8);out->policy=pocket_token_word(d,8);out->string_offset=20;out->format=5;
  out->work_source=(int32_t)pocket_token_word(d,12);return 1;
 }
 if(size>=20 && (pocket_token_word(d,8)&0x00400000u) && pocket_token_string(d,size,16)){
  memcpy(&out->identity,d,8);out->policy=pocket_token_word(d,8);out->string_offset=16;out->format=4;
  out->work_source=(int32_t)pocket_token_word(d,12);return 1;
 }
 if(size>=16 && (pocket_token_word(d,8)&0x00400000u) && pocket_token_string(d,size,12)){
  memcpy(&out->identity,d,8);out->policy=pocket_token_word(d,8);out->string_offset=12;out->format=3;return 1;
 }
 if(pocket_token_string(d,size,4)){out->string_offset=4;out->format=0;return 1;}
 if(pocket_token_string(d,size,8)){out->string_offset=8;out->format=1;out->work_source=(int32_t)pocket_token_word(d,4);return 1;}
 return 0;
}
