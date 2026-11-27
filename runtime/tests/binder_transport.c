// SPDX-License-Identifier: GPL-3.0-only
// Exercise the production ioctl transformer with a bounded, fake Binder driver.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/android/binder.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#define PATH_LIMIT 4096
struct {char root[1024];} pocket_loader_config;
static size_t root_size;
static int guest_identity=1;
static unsigned guest_uid[3]={10017,10017,10017},guest_gid[3];
static unsigned kernel_calls,seen,active_maps,maximum_copy,checks;
static int kernel_error;
static int fd_flags,finish_bootstrap_on_wait;
static unsigned bootstrap_waits;
static size_t consume_limit,reply_bytes;
static unsigned char scripted[512];static size_t scripted_size;static int consume_none;
static void (*inspect)(const struct binder_transaction_data*,uint32_t,const void*);
struct Allocation {long address;size_t size;};
static struct Allocation allocations[2048];
static long raw(long nr,long a,long b,long c,long d,long e,long f);
static long copy_from_user(void*out,uintptr_t source,size_t size) {
 assert(size<=PATH_LIMIT);if(size>maximum_copy)maximum_copy=(unsigned)size;
 if(size && source<4096)return -EFAULT;
 memcpy(out,(const void*)source,size);return 0;
}
static long copy_to_user(uintptr_t target,const char*source,size_t size) {
 assert(size<=PATH_LIMIT);if(size>maximum_copy)maximum_copy=(unsigned)size;
 if(size && target<4096)return -EFAULT;
 memcpy((void*)target,source,size);return 0;
}
static size_t length(const char*s,size_t limit){return strnlen(s,limit);}
#include "../loader/binder_runtime.h"

static long raw(long nr,long a,long b,long c,long d,long e,long f) {
 if(nr==__NR_ioctl && b==BINDER_WRITE_READ) {
  kernel_calls++;struct binder_write_read*rw=(void*)c;
  size_t end=consume_none?rw->write_consumed:consume_limit && consume_limit<rw->write_size?consume_limit:rw->write_size;
  for(size_t at=rw->write_consumed;at<end;) {
   uint32_t command;memcpy(&command,(char*)(uintptr_t)rw->write_buffer+at,4);at+=4;
   size_t size=_IOC_SIZE(command);assert(size<=end-at);
   if(command==BC_TRANSACTION || command==BC_TRANSACTION_SG) {
    struct binder_transaction_data tx;memcpy(&tx,(char*)(uintptr_t)rw->write_buffer+at,sizeof(tx));
    seen++;if(inspect)inspect(&tx,command,(char*)(uintptr_t)rw->write_buffer+at);
   }
   at+=size;
  }
  rw->write_consumed=end;
  if(scripted_size){assert(scripted_size<=rw->read_size);memcpy((void*)(uintptr_t)rw->read_buffer,scripted,scripted_size);rw->read_consumed=scripted_size;scripted_size=0;return kernel_error;}
  rw->read_consumed=reply_bytes;
  assert(reply_bytes<=rw->read_size);
  for(size_t at=0;at<reply_bytes;at+=4){uint32_t noop=BR_NOOP;memcpy((char*)(uintptr_t)rw->read_buffer+at,&noop,4);}
  return kernel_error;
 }
 if(nr==__NR_mmap) {
  long result=syscall(nr,a,b,c,d,e,f);if(result<0)return -errno;
  unsigned i;for(i=0;i<2048 && allocations[i].address;i++){}assert(i<2048);
  allocations[i]=(struct Allocation){result,(size_t)b};active_maps++;return result;
 }
 if(nr==__NR_munmap) {
  unsigned i;for(i=0;i<2048 && allocations[i].address!=a;i++){}assert(i<2048 && allocations[i].size==(size_t)b);
  long result=syscall(nr,a,b,c,d,e,f);assert(!result);allocations[i].address=0;active_maps--;return result;
 }
 if(nr==__NR_fcntl && a==70) {
  if(b==F_GETFL)return fd_flags;
  assert(b==F_SETFL);fd_flags=(int)c;return 0;
 }
 if(nr==__NR_nanosleep && finish_bootstrap_on_wait) {
  bootstrap_waits++;
  fd_flags&=~O_NONBLOCK;
  binder_find(70,0)->state=2;
  kernel_error=0;
  reply_bytes=4;
  return 0;
 }
 if(nr==__NR_getpid || nr==__NR_gettid || nr==__NR_clock_gettime || nr==__NR_nanosleep || nr==__NR_ppoll) {
  long result=syscall(nr,a,b,c,d,e,f);return result<0?-errno:result;
 }
 return -ENOSYS; // The harness never opens or writes production identity files.
}
static void reset(void) {
 assert(!active_maps);kernel_calls=seen=maximum_copy=0;kernel_error=0;consume_limit=reply_bytes=0;inspect=NULL;
 fd_flags=O_RDWR;finish_bootstrap_on_wait=0;bootstrap_waits=0;
 memset(binder_fds,0,sizeof(binder_fds));memset(binder_host_threads,0,sizeof(binder_host_threads));binder_owner_pid=getpid();
 memset(binder_deaths,0,sizeof(binder_deaths));binder_death_withheld=0;binder_death_cursor=0;scripted_size=0;
 struct PocketBinderFd*f=binder_find(70,1);assert(f);f->state=2;f->manager=42;
}
static void word(unsigned char*bytes,size_t at,uint32_t value){memcpy(bytes+at,&value,4);}
static size_t parcel(unsigned char*bytes,unsigned format) {
 const char*name="android.content.pm.IPackageManager";memset(bytes,0,256);
 size_t at=0;
 if(format>=3){uint64_t identity=((uint64_t)10017<<32)|(uint32_t)getpid();memcpy(bytes,&identity,8);at=8;}
 word(bytes,at,format>=3?0x00400000:0x80000000u);at+=4;
 if(format==1 || format==2 || format==4 || format==5){word(bytes,at,0xffffffffu);at+=4;}
 if(format==2 || format==5){word(bytes,at,0x53595354);at+=4;}
 size_t n=strlen(name);word(bytes,at,(uint32_t)n);at+=4;
 for(size_t i=0;i<=n;i++){bytes[at++]=(unsigned char)name[i];bytes[at++]=0;}
 return (at+3)&~(size_t)3;
}
static size_t command(unsigned char*bytes,uint32_t type,struct binder_transaction_data tx) {
 memcpy(bytes,&type,4);memcpy(bytes+4,&tx,sizeof(tx));
 if(type==BC_TRANSACTION_SG){binder_size_t n=32768;memcpy(bytes+4+sizeof(tx),&n,sizeof(n));}
 return 4+_IOC_SIZE(type);
}
static unsigned expected_format;
static uintptr_t original_data;
static size_t original_size;
static void prefixed(const struct binder_transaction_data*tx,uint32_t type,const void*command_bytes) {
 struct PocketBinderToken token;
 assert(pocket_binder_token((void*)(uintptr_t)tx->data.ptr.buffer,tx->data_size,&token));
 assert(token.format==expected_format && token.identity==(((uint64_t)10017<<32)|(uint32_t)getpid()));
 assert(tx->data_size==original_size+8 && tx->data.ptr.buffer!=original_data && tx->target.handle==42);
 if(type==BC_TRANSACTION_SG){struct binder_transaction_data_sg sg;memcpy(&sg,command_bytes,sizeof(sg));assert(sg.buffers_size==32768);}
}
static void unchanged(const struct binder_transaction_data*tx,uint32_t type,const void*bytes) {
 (void)type;(void)bytes;assert(tx->data.ptr.buffer==original_data && tx->data_size==original_size);
}
static void objects(const struct binder_transaction_data*tx,uint32_t type,const void*bytes) {
 prefixed(tx,type,bytes);const binder_size_t*offsets=(void*)(uintptr_t)tx->data.ptr.offsets;
 assert(tx->offsets_size==2*sizeof(*offsets) && offsets[0]==136 && offsets[1]==176);
 assert(!memcmp((char*)(uintptr_t)tx->data.ptr.buffer+136,(void*)(original_data+128),sizeof(struct binder_buffer_object)));
 assert(!memcmp((char*)(uintptr_t)tx->data.ptr.buffer+176,(void*)(original_data+168),sizeof(struct binder_fd_array_object)));
}
static void flat_object(const struct binder_transaction_data*tx,uint32_t type,const void*bytes) {
 prefixed(tx,type,bytes);const binder_size_t*offset=(void*)(uintptr_t)tx->data.ptr.offsets;
 assert(tx->offsets_size==sizeof(*offset) && *offset==136);
 assert(!memcmp((char*)(uintptr_t)tx->data.ptr.buffer+136,(void*)(original_data+128),sizeof(struct flat_binder_object)));
}
static size_t death_command(unsigned char*bytes,uint32_t type,uint32_t handle,binder_uintptr_t cookie) {
 memcpy(bytes,&type,4);
 if(type==BC_CLEAR_DEATH_NOTIFICATION || type==BC_REQUEST_DEATH_NOTIFICATION){struct binder_handle_cookie v={handle,cookie};memcpy(bytes+4,&v,sizeof(v));}
 else memcpy(bytes+4,&cookie,sizeof(cookie));
 return 4+_IOC_SIZE(type);
}
static size_t script(uint32_t type,binder_uintptr_t cookie) {
 size_t at=scripted_size;memcpy(scripted+at,&type,4);at+=4;
 if(_IOC_SIZE(type)){memcpy(scripted+at,&cookie,sizeof(cookie));at+=sizeof(cookie);}
 return scripted_size=at;
}
// Runs one ioctl writing "out" (may be empty) and returns the delivered
// return commands as a compact string such as "D C N" (dead, clear-done, noop).
static const char* exchange(int fd,const unsigned char*out,size_t n) {
 static unsigned char in[512];static char seen_text[64];struct binder_write_read rw={.write_size=n,.write_buffer=(uintptr_t)out,.read_size=sizeof(in),.read_buffer=(uintptr_t)in};
 assert(binder_ioctl(fd,(uintptr_t)&rw)==0 && rw.write_consumed==n && !active_maps);
 size_t t=0;
 for(size_t at=0;at<rw.read_consumed;){uint32_t c;memcpy(&c,in+at,4);at+=4+_IOC_SIZE(c);
  seen_text[t++]=c==BR_DEAD_BINDER?'D':c==BR_CLEAR_DEATH_NOTIFICATION_DONE?'C':c==BR_NOOP?'N':'?';}
 seen_text[t]=0;return seen_text;
}
int main(void) {
 unsigned char payload[16384],before[sizeof(payload)],commands[1024],saved_commands[1024],incoming[8192];
 for(unsigned format=0;format<6;format++) {
  reset();original_size=parcel(payload,format);original_data=(uintptr_t)payload;memcpy(before,payload,sizeof(payload));
  struct binder_transaction_data tx={0};tx.target.handle=42;tx.code=5;tx.data_size=original_size;tx.data.ptr.buffer=original_data;
  size_t n=command(commands,BC_TRANSACTION,tx);memcpy(saved_commands,commands,n);
  struct binder_write_read rw={.write_size=n,.write_buffer=(uintptr_t)commands};
  expected_format=format+3;inspect=format<3?prefixed:unchanged;
  assert(binder_ioctl(70,(uintptr_t)&rw)==0 && seen==1 && rw.write_consumed==n && !active_maps);
  assert(rw.write_buffer==(uintptr_t)commands && !memcmp(before,payload,sizeof(payload)) && !memcmp(commands,saved_commands,n));checks++;
 }
 reset();original_size=256;parcel(payload,2);original_data=(uintptr_t)payload;expected_format=5;
 struct binder_buffer_object parent={.hdr={BINDER_TYPE_PTR},.buffer=0x12345000,.length=4096};
 struct binder_fd_array_object array={.hdr={BINDER_TYPE_FDA},.num_fds=2,.parent=0,.parent_offset=16};
 memcpy(payload+128,&parent,sizeof(parent));memcpy(payload+168,&array,sizeof(array));binder_size_t offsets[]={128,168};
 struct binder_transaction_data tx={0};tx.target.handle=0;tx.code=5;tx.flags=TF_ONE_WAY;tx.data_size=256;tx.data.ptr.buffer=original_data;tx.offsets_size=sizeof(offsets);tx.data.ptr.offsets=(uintptr_t)offsets;
 size_t n=command(commands,BC_TRANSACTION_SG,tx);struct binder_write_read rw={.write_size=n,.write_buffer=(uintptr_t)commands};inspect=objects;
 assert(binder_ioctl(70,(uintptr_t)&rw)==0 && seen==1 && !active_maps && offsets[0]==128 && offsets[1]==168);checks++;
 const uint32_t flat_types[]={BINDER_TYPE_BINDER,BINDER_TYPE_WEAK_BINDER,BINDER_TYPE_HANDLE,BINDER_TYPE_WEAK_HANDLE,BINDER_TYPE_FD};
 for(unsigned i=0;i<sizeof(flat_types)/sizeof(*flat_types);i++) {
  reset();struct flat_binder_object object={.hdr={flat_types[i]},.flags=0x123,.binder=77,.cookie=0x12345678};binder_size_t offset=128;
  memcpy(payload+128,&object,sizeof(object));tx.target.handle=42;tx.offsets_size=sizeof(offset);tx.data.ptr.offsets=(uintptr_t)&offset;inspect=flat_object;
  n=command(commands,BC_TRANSACTION,tx);rw=(struct binder_write_read){.write_size=n,.write_buffer=(uintptr_t)commands};
  assert(binder_ioctl(70,(uintptr_t)&rw)==0 && seen==1 && !active_maps && offset==128);checks++;
 }
 reset();tx.code=0x5f444d50;tx.target.handle=42;tx.offsets_size=0;inspect=unchanged;n=command(commands,BC_TRANSACTION,tx);rw=(struct binder_write_read){.write_size=n,.write_buffer=(uintptr_t)commands};
 assert(binder_ioctl(70,(uintptr_t)&rw)==0 && seen==1 && !active_maps);checks++;
 reset();tx.code=5;original_size=sizeof(payload);memset(payload,0,sizeof(payload));parcel(payload,2);tx.data_size=sizeof(payload);inspect=prefixed;
 n=command(commands,BC_TRANSACTION,tx);rw=(struct binder_write_read){.write_size=n,.write_buffer=(uintptr_t)commands};
 assert(binder_ioctl(70,(uintptr_t)&rw)==0 && maximum_copy==PATH_LIMIT && !active_maps);checks++;
 reset();reply_bytes=sizeof(incoming);rw=(struct binder_write_read){.read_size=sizeof(incoming),.read_buffer=(uintptr_t)incoming};
 assert(binder_ioctl(70,(uintptr_t)&rw)==0 && rw.read_consumed==sizeof(incoming) && maximum_copy==PATH_LIMIT && !active_maps);checks++;
 // A partial kernel write must leave original pointers available for retry.
 reset();original_size=parcel(payload,2);tx.data_size=original_size;inspect=prefixed;
 n=command(commands,BC_TRANSACTION,tx);command(commands+n,BC_TRANSACTION,tx);consume_limit=n;kernel_error=-EAGAIN;
 rw=(struct binder_write_read){.write_size=2*n,.write_buffer=(uintptr_t)commands};
 assert(binder_ioctl(70,(uintptr_t)&rw)==-EAGAIN && rw.write_consumed==n && seen==1 && !active_maps);
 consume_limit=0;kernel_error=0;assert(binder_ioctl(70,(uintptr_t)&rw)==0 && rw.write_consumed==2*n && seen==2 && !active_maps);checks++;
 // The looper gets EAGAIN during bootstrap after its outgoing commands were consumed.
 reset();inspect=prefixed;kernel_error=-EAGAIN;fd_flags|=O_NONBLOCK;finish_bootstrap_on_wait=1;
 struct PocketBinderFd *bootstrap_fd=binder_find(70,0);
 bootstrap_fd->state=1;bootstrap_fd->bootstrap_set_nonblock=1;
 rw=(struct binder_write_read){.write_size=n,.write_buffer=(uintptr_t)commands,.read_size=sizeof(incoming),.read_buffer=(uintptr_t)incoming};
 assert(binder_ioctl(70,(uintptr_t)&rw)==0 && kernel_calls==2 && bootstrap_waits==1);
 assert(rw.write_consumed==n && rw.read_consumed==4 && seen==1 && !active_maps);checks++;
 // EAGAIN may be observed after bootstrap has already restored blocking mode.
 reset();kernel_error=-EAGAIN;binder_find(70,0)->bootstrap_set_nonblock=1;
 rw=(struct binder_write_read){.read_size=sizeof(incoming),.read_buffer=(uintptr_t)incoming};
 assert(binder_ioctl(70,(uintptr_t)&rw)==-EAGAIN && kernel_calls==2 && bootstrap_waits==0);checks++;
 // A descriptor explicitly set nonblocking after bootstrap still gets EAGAIN.
 reset();kernel_error=-EAGAIN;fd_flags|=O_NONBLOCK;binder_find(70,0)->bootstrap_set_nonblock=1;
 assert(binder_ioctl(70,(uintptr_t)&rw)==-EAGAIN && kernel_calls==1 && bootstrap_waits==0);checks++;
 // Bootstrap on an already nonblocking fd leaves its mode and reads alone.
 reset();fd_flags|=O_NONBLOCK;bootstrap_fd=binder_find(70,0);bootstrap_fd->state=1;
 assert(binder_bootstrap(70,bootstrap_fd)==-ENOSYS && !bootstrap_fd->bootstrap_set_nonblock && fd_flags==(O_RDWR|O_NONBLOCK));
 kernel_calls=0;kernel_error=-EAGAIN;
 assert(binder_ioctl(70,(uintptr_t)&rw)==-EAGAIN && kernel_calls==1 && bootstrap_waits==0);checks++;
 reset();struct binder_transaction_data bad=tx;bad.data.ptr.buffer=1;command(commands,BC_TRANSACTION,bad);command(commands+n,BC_TRANSACTION,tx);
 rw=(struct binder_write_read){.write_size=2*n,.write_consumed=n,.write_buffer=(uintptr_t)commands};inspect=prefixed;
 assert(binder_ioctl(70,(uintptr_t)&rw)==0 && seen==1 && !active_maps);checks++;
 reset();rw=(struct binder_write_read){.write_size=2*n,.write_consumed=4,.write_buffer=(uintptr_t)commands};
 assert(binder_ioctl(70,(uintptr_t)&rw)==-EINVAL && kernel_calls==0 && !active_maps);checks++;
 // Malformed objects fail before any kernel transaction or input mutation.
 const binder_size_t invalid[]={1,original_size-2,UINT64_MAX,original_size+4};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++) {
  reset();binder_size_t offset=invalid[i];tx.offsets_size=sizeof(offset);tx.data.ptr.offsets=(uintptr_t)&offset;
  n=command(commands,BC_TRANSACTION,tx);rw=(struct binder_write_read){.write_size=n,.write_buffer=(uintptr_t)commands};
  assert(binder_ioctl(70,(uintptr_t)&rw)==-EINVAL && kernel_calls==0 && !active_maps && offset==invalid[i]);checks++;
 }
 reset();tx.offsets_size=0;tx.data_size=SIZE_MAX;n=command(commands,BC_TRANSACTION,tx);rw=(struct binder_write_read){.write_size=n,.write_buffer=(uintptr_t)commands};
 assert(binder_ioctl(70,(uintptr_t)&rw)==-E2BIG && kernel_calls==0 && !active_maps);checks++;
 reset();tx.data_size=original_size;tx.data.ptr.buffer=1;n=command(commands,BC_TRANSACTION,tx);rw=(struct binder_write_read){.write_size=n,.write_buffer=(uintptr_t)commands};
 assert(binder_ioctl(70,(uintptr_t)&rw)==-EFAULT && kernel_calls==0 && !active_maps);checks++;
 reset();tx.data.ptr.buffer=original_data;struct binder_transaction_data broken=tx;broken.data.ptr.buffer=1;
 n=command(commands,BC_TRANSACTION,tx);command(commands+n,BC_TRANSACTION,broken);rw=(struct binder_write_read){.write_size=2*n,.write_buffer=(uintptr_t)commands};
 assert(binder_ioctl(70,(uintptr_t)&rw)==-EFAULT && kernel_calls==0 && !active_maps);checks++;
 reset();tx.data_size=256;tx.offsets_size=7;tx.data.ptr.offsets=(uintptr_t)offsets;n=command(commands,BC_TRANSACTION,tx);rw=(struct binder_write_read){.write_size=n,.write_buffer=(uintptr_t)commands};
 assert(binder_ioctl(70,(uintptr_t)&rw)==-EINVAL && kernel_calls==0 && !active_maps);checks++;
 reset();tx.offsets_size=8;tx.data.ptr.offsets=1;n=command(commands,BC_TRANSACTION,tx);rw=(struct binder_write_read){.write_size=n,.write_buffer=(uintptr_t)commands};
 assert(binder_ioctl(70,(uintptr_t)&rw)==-EFAULT && kernel_calls==0 && !active_maps);checks++;
 // A HIDL-style buffer object can resemble a one-character interface token.
 reset();struct binder_buffer_object hidl={.hdr={BINDER_TYPE_PTR},.flags=1,.buffer=0x700000000040,.length=64};binder_size_t zero=0;
 memcpy(payload,&hidl,sizeof(hidl));original_size=256;tx.data_size=256;tx.data.ptr.buffer=original_data;tx.data.ptr.offsets=(uintptr_t)&zero;inspect=unchanged;
 n=command(commands,BC_TRANSACTION,tx);rw=(struct binder_write_read){.write_size=n,.write_buffer=(uintptr_t)commands};
 assert(binder_ioctl(70,(uintptr_t)&rw)==0 && seen==1 && !active_maps);checks++;
 reset();assert(binder_host_scope(1)==0);__u64 args[6]={70,BINDER_WRITE_READ,(uintptr_t)&rw};
 assert(!binder_dispatch(__NR_ioctl,args) && args[0]==70 && !kernel_calls);assert(binder_host_scope(0)==0);checks++;
 reset();guest_identity=0;assert(!binder_dispatch(__NR_ioctl,args) && !kernel_calls);guest_identity=1;checks++;
 assert(binder_copy_from_user(payload,UINTPTR_MAX-4,8)==-EFAULT);assert(binder_copy_to_user(UINTPTR_MAX-4,payload,8)==-EFAULT);checks++;
 // Death notifications. Fd 71 has no direct-client bootstrap, like libbinder.
 {
  const binder_uintptr_t X=0xb400006e116097d0ull,Y=0xb400006e1160a790ull;unsigned char out[64];size_t m;
  // Rust Binder dead-owner sequence recorded in acore (trace run 178): early
  // DONE before BC_DEAD_BINDER_DONE, second DONE after it. Deliver exactly one.
  reset();script(BR_DEAD_BINDER,X);assert(!strcmp(exchange(71,out,0),"D"));
  m=death_command(out,BC_CLEAR_DEATH_NOTIFICATION,34,X);script(BR_CLEAR_DEATH_NOTIFICATION_DONE,X);script(BR_NOOP,0);
  assert(!strcmp(exchange(71,out,m),"N") && binder_death_withheld==1);
  m=death_command(out,BC_DEAD_BINDER_DONE,0,X);script(BR_CLEAR_DEATH_NOTIFICATION_DONE,X);
  assert(!strcmp(exchange(71,out,m),"C"));
  for(unsigned i=0;i<POCKET_BINDER_DEATHS;i++)assert(!binder_deaths[i].clears && !binder_deaths[i].dead);
  // A third DONE for the settled cookie is withheld too.
  script(BR_CLEAR_DEATH_NOTIFICATION_DONE,X);assert(!strcmp(exchange(71,out,0),"") && binder_death_withheld==2);checks++;
  // C Binder / fixed Rust: the single DONE follows BC_DEAD_BINDER_DONE.
  reset();script(BR_DEAD_BINDER,X);assert(!strcmp(exchange(71,out,0),"D"));
  m=death_command(out,BC_CLEAR_DEATH_NOTIFICATION,34,X);assert(!strcmp(exchange(71,out,m),""));
  m=death_command(out,BC_DEAD_BINDER_DONE,0,X);script(BR_CLEAR_DEATH_NOTIFICATION_DONE,X);
  assert(!strcmp(exchange(71,out,m),"C") && !binder_death_withheld);checks++;
  // Both DONEs in one read after BC_DEAD_BINDER_DONE: still exactly one.
  reset();script(BR_DEAD_BINDER,X);assert(!strcmp(exchange(71,out,0),"D"));
  m=death_command(out,BC_CLEAR_DEATH_NOTIFICATION,34,X);m+=death_command(out+m,BC_DEAD_BINDER_DONE,0,X);
  script(BR_CLEAR_DEATH_NOTIFICATION_DONE,X);script(BR_CLEAR_DEATH_NOTIFICATION_DONE,X);
  assert(!strcmp(exchange(71,out,m),"C") && binder_death_withheld==1);checks++;
  // Ordinary unlink of a live binder, DONE read by the clearing ioctl itself,
  // and an unrelated cookie interleaved; nothing is withheld.
  reset();m=death_command(out,BC_CLEAR_DEATH_NOTIFICATION,30,Y);script(BR_CLEAR_DEATH_NOTIFICATION_DONE,Y);script(BR_NOOP,0);
  assert(!strcmp(exchange(71,out,m),"CN") && !binder_death_withheld);checks++;
  // A cookie this process never cleared through the loader passes unchanged.
  reset();script(BR_CLEAR_DEATH_NOTIFICATION_DONE,Y);assert(!strcmp(exchange(71,out,0),"C"));checks++;
  // An unconsumed clear is rolled back; a later DONE for it is not invented.
  reset();m=death_command(out,BC_CLEAR_DEATH_NOTIFICATION,30,Y);consume_none=1;kernel_error=-EAGAIN;
  {struct binder_write_read rw={.write_size=m,.write_buffer=(uintptr_t)out};assert(binder_ioctl(71,(uintptr_t)&rw)==-EAGAIN && rw.write_consumed==0);}
  consume_none=0;kernel_error=0;
  script(BR_CLEAR_DEATH_NOTIFICATION_DONE,Y);assert(!strcmp(exchange(71,out,0),"") && binder_death_withheld==1);
  for(unsigned i=0;i<POCKET_BINDER_DEATHS;i++)assert(!binder_deaths[i].clears && !binder_deaths[i].dead);checks++;
  // Direct bootstrap clients (fd 70) get the same guard; close forgets state.
  reset();script(BR_DEAD_BINDER,X);assert(!strcmp(exchange(70,out,0),"D"));
  m=death_command(out,BC_CLEAR_DEATH_NOTIFICATION,34,X);script(BR_CLEAR_DEATH_NOTIFICATION_DONE,X);
  assert(!strcmp(exchange(70,out,m),"") && binder_death_withheld==1);
  binder_closed(70);for(unsigned i=0;i<POCKET_BINDER_DEATHS;i++)assert(!binder_deaths[i].key);checks++;
 }
 // Retired records never displace active ones when the table is full.
 {
  unsigned char out[64];size_t m;reset();
  for(unsigned i=0;i<POCKET_BINDER_DEATHS;i++){m=death_command(out,BC_CLEAR_DEATH_NOTIFICATION,1,0x1000+i);assert(!strcmp(exchange(71,out,m),""));}
  for(unsigned i=0;i<POCKET_BINDER_DEATHS;i+=2){script(BR_CLEAR_DEATH_NOTIFICATION_DONE,0x1000+i);assert(!strcmp(exchange(71,out,0),"C"));}
  script(BR_DEAD_BINDER,0x9000);assert(!strcmp(exchange(71,out,0),"D"));
  for(unsigned i=1;i<POCKET_BINDER_DEATHS;i+=2){script(BR_CLEAR_DEATH_NOTIFICATION_DONE,0x1000+i);assert(!strcmp(exchange(71,out,0),"C"));}
  assert(!binder_death_withheld);checks++;
 }
 printf("PASS: raw Binder credential transport %u cases; original buffers, SG objects, partial writes, bootstrap read retry, host scope, bounds, cleanup and death-notification DONE guard\n",checks);
}
