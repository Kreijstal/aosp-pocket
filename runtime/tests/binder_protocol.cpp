// SPDX-License-Identifier: GPL-3.0-only
#include "../api28/binder_token.h"
#include "../loader/binder_identity.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void word(unsigned char*b,size_t at,uint32_t n){memcpy(b+at,&n,4);}
static size_t parcel(unsigned char*b,unsigned format) {
 memset(b,0,256);const char*name="android.os.IServiceManager";size_t at=4;
 if(format==3 || format==4){uint64_t id=((uint64_t)10017<<32)|123;memcpy(b,&id,8);word(b,8,0x00400000);at=12;
  if(format==4){word(b,12,29018);at=16;}}
 else{word(b,0,0x80000000);if(format){at=8;if(format==2){word(b,8,0x53595354);at=12;}}}
 size_t n=strlen(name);word(b,at,(uint32_t)n);for(size_t i=0;i<n;i++)b[at+4+i*2]=(unsigned char)name[i];return at+4+(n+1)*2;
}
int main(){unsigned char bytes[256];PocketBinderToken token;unsigned cases=0;
 for(unsigned format=0;format<5;format++){size_t n=parcel(bytes,format);assert(pocket_binder_token(bytes,n,&token));assert(token.format==format);cases++;
  if(format==4){assert(token.identity==(((uint64_t)10017<<32)|123));assert(token.work_source==29018);cases++;
   word(bytes,12,0xffffffff);assert(pocket_binder_token(bytes,n,&token));assert(token.work_source==-1);cases++;}
  assert(!pocket_binder_token(bytes,n-1,&token));cases++;
 }
 size_t n=parcel(bytes,2);assert(pocket_binder_token(bytes,n,&token)&&token.format==2);cases++;
 word(bytes,12,0xffffffff);assert(!pocket_binder_token(bytes,n,&token));cases++;
 assert(!pocket_binder_token(bytes,3,&token));cases++;
 assert(pocket_process_start("broken",6)==0);cases++;
 char stat[4096];FILE*f=fopen("/proc/self/stat","r");assert(f);size_t count=fread(stat,1,sizeof(stat),f);fclose(f);
 assert(pocket_process_start(stat,count)>0);cases++;
 printf("PASS: Binder token/identity protocol %u cases\n",cases);
}
