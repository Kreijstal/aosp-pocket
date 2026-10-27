// SPDX-License-Identifier: GPL-3.0-only
// Direct Binder clients retain real kernel handles, buffer ownership, and
// reference counting. Only their context-manager target is redirected.
#pragma once
#include <linux/android/binder.h>
#include <poll.h>
#include "binder_identity.h"
#define POCKET_BINDER_FDS 32
#define POCKET_BINDER_CAP 65536
struct PocketBinderFd {int key,state;uint32_t manager;uintptr_t node,cookie;};
static struct PocketBinderFd binder_fds[POCKET_BINDER_FDS];
static struct {int tid,depth;} binder_host_threads[64];
static int binder_owner_pid;
static void binder_reset_after_fork(void) {
 int pid=(int)raw(__NR_getpid,0,0,0,0,0,0);
 if(binder_owner_pid!=pid){memset(binder_fds,0,sizeof(binder_fds));memset(binder_host_threads,0,sizeof(binder_host_threads));binder_owner_pid=pid;}
}
static long binder_host_scope(unsigned enabled) {
 binder_reset_after_fork();int tid=(int)raw(__NR_gettid,0,0,0,0,0,0);
 for(unsigned i=0;i<64;i++) {
  int key=__atomic_load_n(&binder_host_threads[i].tid,__ATOMIC_ACQUIRE);
  if(key!=tid && (key || !enabled))continue;
  if(!key){int zero=0;if(!__atomic_compare_exchange_n(&binder_host_threads[i].tid,&zero,tid,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))continue;}
  if(enabled){if(binder_host_threads[i].depth>=32)return -EOVERFLOW;binder_host_threads[i].depth++;}
  else if(binder_host_threads[i].depth>0)binder_host_threads[i].depth--;
  if(!binder_host_threads[i].depth)__atomic_store_n(&binder_host_threads[i].tid,0,__ATOMIC_RELEASE);
  return 0;
 }
 return enabled?-EUSERS:0;
}
static int binder_is_host(void) {
 int tid=(int)raw(__NR_gettid,0,0,0,0,0,0);
 for(unsigned i=0;i<64;i++)if(__atomic_load_n(&binder_host_threads[i].tid,__ATOMIC_ACQUIRE)==tid && binder_host_threads[i].depth)return 1;
 return 0;
}
static size_t binder_decimal(char*out,unsigned n) {
 char digits[16];size_t at=0;do{digits[at++]='0'+n%10;n/=10;}while(n);
 size_t count=at;while(at)*out++=digits[--at];return count;
}
static void binder_publish_identity(void) {
 if(!guest_identity)return;
 char path[1280];memcpy(path,pocket_loader_config.root,root_size);
 const char directory[]="/.pocket-identity";memcpy(path+root_size,directory,sizeof(directory));
 if(raw(__NR_mkdirat,AT_FDCWD,(long)path,0700,0,0,0)<0) {/* Existing directory is normal. */}
 char stat[4096];long fd=raw(__NR_openat,AT_FDCWD,(long)"/proc/self/stat",O_RDONLY|O_CLOEXEC,0,0,0);if(fd<0)return;
 long count=raw(__NR_read,fd,(long)stat,sizeof(stat),0,0,0);raw(__NR_close,fd,0,0,0,0,0);
 if(count<=0)return;uint64_t start=pocket_process_start(stat,(size_t)count);if(!start)return;
 unsigned pid=(unsigned)raw(__NR_getpid,0,0,0,0,0,0);size_t at=root_size+sizeof(directory)-1;
 path[at++]='/';at+=binder_decimal(path+at,pid);path[at]=0;
 struct PocketBinderIdentity record={POCKET_IDENTITY_MAGIC,pid,guest_uid[1],guest_gid[1],start};
 fd=raw(__NR_openat,AT_FDCWD,(long)path,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600,0,0);
 if(fd>=0){raw(__NR_write,fd,(long)&record,sizeof(record),0,0,0);raw(__NR_close,fd,0,0,0,0,0);}
}
static struct PocketBinderFd* binder_find(int fd,int create) {
 binder_reset_after_fork();if(fd<0 || fd==INT32_MAX)return 0;
 for(unsigned i=0;i<POCKET_BINDER_FDS;i++)if(__atomic_load_n(&binder_fds[i].key,__ATOMIC_ACQUIRE)==fd+1)return binder_fds+i;
 if(create)for(unsigned i=0;i<POCKET_BINDER_FDS;i++){int zero=0;
  if(__atomic_compare_exchange_n(&binder_fds[i].key,&zero,fd+1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return binder_fds+i;
 }
 return 0;
}
static void binder_closed(int fd) {struct PocketBinderFd*f=binder_find(fd,0);if(f){f->manager=0;f->state=0;f->node=f->cookie=0;__atomic_store_n(&f->key,0,__ATOMIC_RELEASE);}}
static int64_t binder_time(void){struct timespec ts;if(raw(__NR_clock_gettime,1,(long)&ts,0,0,0,0)<0)return 0;return ts.tv_sec*1000+ts.tv_nsec/1000000;}
static long binder_write(int fd,const void*data,size_t size) {
 size_t at=0;for(unsigned tries=0;at<size && tries<64;tries++){
  struct binder_write_read rw={0};rw.write_size=size-at;rw.write_buffer=(uintptr_t)data+at;
  long rc=raw(__NR_ioctl,fd,BINDER_WRITE_READ,(long)&rw,0,0,0);if(rw.write_consumed>size-at)return -EPROTO;at+=rw.write_consumed;
  if(rc<0 && rc!=-EINTR)return rc;
 }
 return at==size?0:-EIO;
}
static long binder_pair_command(int fd,unsigned cmd,uintptr_t value) {
 char bytes[4+sizeof(value)];memcpy(bytes,&cmd,4);memcpy(bytes+4,&value,sizeof(value));return binder_write(fd,bytes,sizeof(bytes));
}
static long binder_hold(int fd,uint32_t handle) {
 uint32_t bytes[]={BC_INCREFS,handle,BC_ACQUIRE,handle};return binder_write(fd,bytes,sizeof(bytes));
}
static long binder_free(int fd,binder_uintptr_t buffer){return binder_pair_command(fd,BC_FREE_BUFFER,buffer);}
static long binder_reply_handle(int fd,const struct binder_transaction_data*tx,uint32_t*handle) {
 if(tx->flags&TF_STATUS_CODE)return -EPROTO;
 if(tx->offsets_size>256 || tx->offsets_size%sizeof(binder_size_t))return -EPROTO;
 binder_size_t offsets[32];long rc=copy_from_user(offsets,tx->data.ptr.offsets,tx->offsets_size);if(rc<0)return rc;
 *handle=0;
 for(unsigned i=0;i<tx->offsets_size/sizeof(binder_size_t);i++) {
  if(offsets[i]>tx->data_size || sizeof(struct flat_binder_object)>tx->data_size-offsets[i])return -EPROTO;
  struct flat_binder_object object;rc=copy_from_user(&object,tx->data.ptr.buffer+offsets[i],sizeof(object));if(rc<0)return rc;
  if(object.hdr.type==BINDER_TYPE_HANDLE && object.handle){*handle=object.handle;return binder_hold(fd,*handle);}
 }
 return 0;
}
static long binder_own_reference(int fd,struct PocketBinderFd*f,unsigned command,const void*bytes) {
 if(command!=BR_INCREFS && command!=BR_ACQUIRE && command!=BR_RELEASE && command!=BR_DECREFS)return 0;
 struct binder_ptr_cookie pair;memcpy(&pair,bytes,sizeof(pair));if(pair.ptr!=f->node || pair.cookie!=f->cookie)return 0;
 if(command==BR_INCREFS || command==BR_ACQUIRE){uint32_t ack=command==BR_INCREFS?BC_INCREFS_DONE:BC_ACQUIRE_DONE;
  char data[4+sizeof(pair)];memcpy(data,&ack,4);memcpy(data+4,&pair,sizeof(pair));long rc=binder_write(fd,data,sizeof(data));if(rc<0)return rc;
 }
 return 1;
}
static long binder_own_callback(int fd,struct PocketBinderFd*f,const struct binder_transaction_data*tx) {
 if(tx->target.ptr!=f->node || tx->cookie!=f->cookie)return 0;
 if(tx->code!=2 || (tx->flags&TF_ONE_WAY))return -EPROTO;
 uint32_t manager;long rc=binder_reply_handle(fd,tx,&manager);if(rc<0 || !manager)return rc<0?rc:-EPROTO;
 f->manager=manager;
 struct binder_transaction_data reply={0};uint32_t ok=0,cmd=BC_REPLY;
 reply.data_size=sizeof(ok);reply.data.ptr.buffer=(uintptr_t)&ok;
 char bytes[4+sizeof(reply)];memcpy(bytes,&cmd,4);memcpy(bytes+4,&reply,sizeof(reply));
 rc=binder_write(fd,bytes,sizeof(bytes));if(rc<0)return rc;
 rc=binder_free(fd,tx->data.ptr.buffer);return rc<0?rc:1;
}
static long binder_exchange(int fd,struct PocketBinderFd*f,struct binder_transaction_data*tx,int need_manager,uint32_t*handle) {
 char outgoing[4+sizeof(*tx)];uint32_t command=BC_TRANSACTION;memcpy(outgoing,&command,4);memcpy(outgoing+4,tx,sizeof(*tx));
 long rc=binder_write(fd,outgoing,sizeof(outgoing));if(rc<0)return rc;
 char input[4096];int got_reply=0;int64_t deadline=binder_time()+6000;
 for(unsigned iterations=0;iterations<256 && binder_time()<deadline;iterations++){
  struct binder_write_read rw={0};rw.read_size=sizeof(input);rw.read_buffer=(uintptr_t)input;
  rc=raw(__NR_ioctl,fd,BINDER_WRITE_READ,(long)&rw,0,0,0);
  if(rc<0){if(rc==-EINTR)continue;if(rc!=-EAGAIN)return rc;
   struct pollfd poller={fd,POLLIN,0};struct timespec wait={0,100000000};raw(__NR_ppoll,(long)&poller,1,(long)&wait,0,0,0);continue;
  }
  if(rw.read_consumed>sizeof(input))return -EPROTO;
  for(size_t at=0;at<rw.read_consumed;){
   if(rw.read_consumed-at<4)return -EPROTO;memcpy(&command,input+at,4);at+=4;
   size_t size=_IOC_SIZE(command);if(size>rw.read_consumed-at)return -EPROTO;
   long own=binder_own_reference(fd,f,command,input+at);if(own<0)return own;
   if(command==BR_REPLY){
    if(size!=sizeof(*tx))return -EPROTO;struct binder_transaction_data response;memcpy(&response,input+at,sizeof(response));
    rc=need_manager?0:binder_reply_handle(fd,&response,handle);long freed=binder_free(fd,response.data.ptr.buffer);
    if(rc<0)return rc;if(freed<0)return freed;got_reply=1;
   }else if(command==BR_TRANSACTION){struct binder_transaction_data callback;if(size!=sizeof(callback))return -EPROTO;memcpy(&callback,input+at,sizeof(callback));
    own=binder_own_callback(fd,f,&callback);if(own<=0)return own<0?own:-EPROTO;
   }else if(command==BR_FAILED_REPLY || command==BR_DEAD_REPLY || command==BR_ERROR)return -EIO;
   at+=size;
  }
  if(got_reply && (!need_manager || f->manager))return 0;
 }
 return -ETIMEDOUT;
}
static long binder_file(const char*name,void*data,size_t limit) {
 char path[1280];size_t n=length(name,128);if(root_size+n>=sizeof(path))return -ENAMETOOLONG;
 memcpy(path,pocket_loader_config.root,root_size);memcpy(path+root_size,name,n+1);
 long fd=raw(__NR_openat,AT_FDCWD,(long)path,O_RDONLY|O_CLOEXEC,0,0,0);if(fd<0)return fd;
 long size=raw(__NR_read,fd,(long)data,limit,0,0,0);raw(__NR_close,fd,0,0,0,0,0);return size;
}
static long binder_bootstrap(int fd,struct PocketBinderFd*f) {
 long memory=raw(__NR_mmap,0,POCKET_BINDER_CAP+64,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(memory<0)return memory;
 char*bytes=(char*)memory;long flags=raw(__NR_fcntl,fd,F_GETFL,0,0,0,0),result=-EPROTO;
 if(flags<0){result=flags;goto done;}
 result=raw(__NR_fcntl,fd,F_SETFL,flags|O_NONBLOCK,0,0,0);if(result<0)goto done;
 f->node=(uintptr_t)f;f->cookie=0x504b4252;
 {uint32_t enter=BC_ENTER_LOOPER;result=binder_write(fd,&enter,4);if(result<0)goto restore;}
 {long n=binder_file("/activityServiceRequest",bytes,POCKET_BINDER_CAP+64);uint64_t size[2];
  if(n<16){result=n<0?n:-EPROTO;goto exit_loop;}memcpy(size,bytes,16);
  if(size[0]>POCKET_BINDER_CAP || size[1]!=0 || size[0]+16!=(uint64_t)n){result=-EPROTO;goto exit_loop;}
  struct binder_transaction_data tx={0};tx.code=1;tx.flags=TF_ACCEPT_FDS;tx.data_size=size[0];tx.data.ptr.buffer=(uintptr_t)(bytes+16);
  uint32_t activity=0;result=binder_exchange(fd,f,&tx,0,&activity);if(result<0 || !activity){if(!result)result=-ENOENT;goto exit_loop;}
  char number[32];long count=binder_file("/trans_code",number,sizeof(number));if(count<=0){result=-ENOENT;goto exit_loop;}
  unsigned code=0;for(long i=0;i<count && number[i]>='0'&&number[i]<='9';i++)code=code*10+(unsigned)(number[i]-'0');
  if(!code || code>4096){result=-EPROTO;goto exit_loop;}
  n=binder_file("/localBroadcastIntent",bytes,POCKET_BINDER_CAP+64);if(n<16){result=n<0?n:-EPROTO;goto exit_loop;}memcpy(size,bytes,16);
  if(size[0]>POCKET_BINDER_CAP || size[1]!=1 || size[0]+24!=(uint64_t)n){result=-EPROTO;goto exit_loop;}
  binder_size_t offset;memcpy(&offset,bytes+16+size[0],sizeof(offset));
  if(offset%4 || offset>size[0] || sizeof(struct flat_binder_object)>size[0]-offset){result=-EPROTO;goto exit_loop;}
  struct flat_binder_object object;memcpy(&object,bytes+16+offset,sizeof(object));
  if(object.hdr.type!=BINDER_TYPE_BINDER){result=-EPROTO;goto exit_loop;}
  object.binder=f->node;object.cookie=f->cookie;memcpy(bytes+16+offset,&object,sizeof(object));
  memset(&tx,0,sizeof(tx));tx.target.handle=activity;tx.code=code;tx.flags=TF_ACCEPT_FDS;tx.data_size=size[0];tx.offsets_size=sizeof(offset);
  tx.data.ptr.buffer=(uintptr_t)(bytes+16);tx.data.ptr.offsets=(uintptr_t)&offset;
  result=binder_exchange(fd,f,&tx,1,0);
 }
 exit_loop:{uint32_t leave=BC_EXIT_LOOPER;(void)binder_write(fd,&leave,4);}
 restore:(void)raw(__NR_fcntl,fd,F_SETFL,flags,0,0,0);
 done:raw(__NR_munmap,memory,POCKET_BINDER_CAP+64,0,0,0,0);return result;
}
static long binder_context(int fd,uint32_t*manager) {
 struct PocketBinderFd*f=binder_find(fd,1);if(!f)return -EMFILE;
 int expected=0;
 if(__atomic_compare_exchange_n(&f->state,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
  long rc=binder_bootstrap(fd,f);__atomic_store_n(&f->state,rc<0?(int)rc:2,__ATOMIC_RELEASE);
 }else {for(unsigned i=0;i<8000 && __atomic_load_n(&f->state,__ATOMIC_ACQUIRE)==1;i++){struct timespec wait={0,1000000};raw(__NR_nanosleep,(long)&wait,0,0,0,0,0);}}
 int state=__atomic_load_n(&f->state,__ATOMIC_ACQUIRE);if(state!=2)return state<0?state:-ETIMEDOUT;
 *manager=f->manager;return *manager?0:-EHOSTDOWN;
}
static long binder_filter_read(int fd,struct PocketBinderFd*f,struct binder_write_read*rw) {
 if(!f || !rw->read_consumed)return 0;
 if(rw->read_consumed>rw->read_size || rw->read_consumed>POCKET_BINDER_CAP)return -E2BIG;
 size_t allocation=rw->read_consumed;
 long mem=raw(__NR_mmap,0,rw->read_consumed,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(mem<0)return mem;
 char*bytes=(char*)mem;long result=copy_from_user(bytes,rw->read_buffer,rw->read_consumed);size_t out=0;
 if(!result)for(size_t at=0;at<rw->read_consumed;){
  if(rw->read_consumed-at<4){result=-EPROTO;break;}uint32_t command;memcpy(&command,bytes+at,4);
  size_t size=_IOC_SIZE(command);if(size>rw->read_consumed-at-4){result=-EPROTO;break;}
  long own=binder_own_reference(fd,f,command,bytes+at+4);
  if(command==BR_TRANSACTION){struct binder_transaction_data tx;if(size!=sizeof(tx)){result=-EPROTO;break;}memcpy(&tx,bytes+at+4,sizeof(tx));own=binder_own_callback(fd,f,&tx);}
  if(own<0){result=own;break;}
  if(!own){for(size_t i=0;i<size+4;i++)bytes[out+i]=bytes[at+i];out+=size+4;}at+=size+4;
 }
 if(!result){result=copy_to_user(rw->read_buffer,bytes,out);if(result>=0){rw->read_consumed=out;result=0;}}
 raw(__NR_munmap,mem,allocation,0,0,0,0);return result;
}
static long binder_ioctl(int fd,uintptr_t user) {
 struct binder_write_read rw;long rc=copy_from_user(&rw,user,sizeof(rw));if(rc<0)return rc;
 if(rw.write_consumed>rw.write_size || rw.write_size>POCKET_BINDER_CAP)return -E2BIG;
 long mem=0;struct binder_write_read original=rw;
 if(rw.write_size){mem=raw(__NR_mmap,0,rw.write_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(mem<0)return mem;
  rc=copy_from_user((void*)mem,rw.write_buffer,rw.write_size);if(rc<0)goto done;
  char*bytes=(char*)mem;
  for(size_t at=0;at<rw.write_size;){uint32_t command;if(rw.write_size-at<4){rc=-EINVAL;goto done;}memcpy(&command,bytes+at,4);at+=4;
   size_t size=_IOC_SIZE(command);if(size>rw.write_size-at){rc=-EINVAL;goto done;}
   if(command==BC_TRANSACTION || command==BC_TRANSACTION_SG){
    if(size<sizeof(struct binder_transaction_data)){rc=-EINVAL;goto done;}
    struct binder_transaction_data tx;memcpy(&tx,bytes+at,sizeof(tx));
    if(!tx.target.handle){uint32_t manager;rc=binder_context(fd,&manager);if(rc<0)goto done;tx.target.handle=manager;memcpy(bytes+at,&tx,sizeof(tx));}
   }
   at+=size;
  }
  rw.write_buffer=(uintptr_t)mem;
 }
 rc=raw(__NR_ioctl,fd,BINDER_WRITE_READ,(long)&rw,0,0,0);
 {long filtered=binder_filter_read(fd,binder_find(fd,0),&rw);if(filtered<0){rc=filtered;goto done;}}
 rw.write_buffer=original.write_buffer;
 {long copied=copy_to_user(user,(const char*)&rw,sizeof(rw));if(copied<0)rc=copied;}
 done:if(mem>0)raw(__NR_munmap,mem,original.write_size,0,0,0,0);return rc;
}
static int binder_dispatch(long nr,__u64*r) {
 if(!guest_identity)return 0;
 if(nr==__NR_close){binder_closed((int)r[0]);r[0]=raw(nr,r[0],0,0,0,0,0);return 1;}
 if(nr==__NR_dup || nr==__NR_dup3 || nr==__NR_fcntl){
  int old=(int)r[0];struct PocketBinderFd*from=binder_find(old,0);
  long result=raw(nr,r[0],r[1],r[2],0,0,0);
  if(result>=0 && (nr!=__NR_fcntl || r[1]==F_DUPFD || r[1]==F_DUPFD_CLOEXEC)){
   if(nr==__NR_dup3)binder_closed((int)result);
   if(from && from->state==2){struct PocketBinderFd*to=binder_find((int)result,1);if(to){to->manager=from->manager;to->node=from->node;to->cookie=from->cookie;to->state=2;}}
  }
  r[0]=result;return 1;
 }
 if(nr!=__NR_ioctl || r[1]!=BINDER_WRITE_READ || binder_is_host())return 0;
 r[0]=binder_ioctl((int)r[0],r[2]);return 1;
}
