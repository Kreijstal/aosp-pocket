// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <jni.h>
#include <linux/fuse.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct Server {int fd;pthread_t thread;uint8_t data[65536];};
static void attr(struct fuse_attr* a,uint64_t inode) {
    memset(a,0,sizeof(*a));a->ino=inode;a->size=inode==FUSE_ROOT_ID?0:65536;
    a->blocks=(a->size+511)/512;a->mode=inode==FUSE_ROOT_ID?(S_IFDIR|0700):(S_IFREG|0600);
    a->nlink=1;a->uid=getuid();a->gid=getgid();a->blksize=4096;
}
static void* serve(void* opaque) {
    struct Server* s=opaque;uint8_t* request=malloc(262144),*reply=malloc(262144);
    if(!request || !reply){free(request);free(reply);return NULL;}
    for(;;) {
        ssize_t n=read(s->fd,request,262144);if(n<0 && errno==EINTR)continue;
        if(n<(ssize_t)sizeof(struct fuse_in_header))break;
        struct fuse_in_header in;memcpy(&in,request,sizeof(in));
        if(in.len!=(uint32_t)n)break;
        struct fuse_out_header out={.len=sizeof(out),.unique=in.unique};
        size_t length=0;void* body=reply+sizeof(out);const uint8_t* args=request+sizeof(in);
        if(in.opcode==FUSE_FORGET || in.opcode==FUSE_BATCH_FORGET)continue;
        switch(in.opcode) {
        case FUSE_LOOKUP: {
            struct fuse_entry_out value={0};value.nodeid=2;value.generation=1;attr(&value.attr,2);
            if(n<=(ssize_t)sizeof(in) || args[n-sizeof(in)-1]!=0 || strcmp((const char*)args,"2"))out.error=-ENOENT;
            else {length=sizeof(value);memcpy(body,&value,length);}break;
        }
        case FUSE_GETATTR: {
            struct fuse_attr_out value={0};attr(&value.attr,in.nodeid);length=sizeof(value);memcpy(body,&value,length);break;
        }
        case FUSE_OPEN: {
            // Cached I/O permits ordinary file-backed mmap, unlike DIRECT_IO.
            struct fuse_open_out value={.fh=2,.open_flags=FOPEN_KEEP_CACHE};
            length=sizeof(value);memcpy(body,&value,length);break;
        }
        case FUSE_READ: {
            if(n<(ssize_t)(sizeof(in)+sizeof(struct fuse_read_in))){out.error=-EINVAL;break;}
            struct fuse_read_in value;memcpy(&value,args,sizeof(value));
            if(value.offset>=sizeof(s->data))break;
            length=value.size;if(length>sizeof(s->data)-value.offset)length=sizeof(s->data)-value.offset;
            memcpy(body,s->data+value.offset,length);break;
        }
        case FUSE_WRITE: {
            if(n<(ssize_t)(sizeof(in)+sizeof(struct fuse_write_in))){out.error=-EINVAL;break;}
            struct fuse_write_in value;memcpy(&value,args,sizeof(value));
            if(value.offset>sizeof(s->data) || value.size>sizeof(s->data)-value.offset || value.size>(size_t)n-sizeof(in)-sizeof(value)){out.error=-EFBIG;break;}
            memcpy(s->data+value.offset,args+sizeof(value),value.size);
            struct fuse_write_out response={.size=value.size};length=sizeof(response);memcpy(body,&response,length);break;
        }
        case FUSE_FSYNC:case FUSE_RELEASE:case FUSE_FLUSH:break;
        default:out.error=-ENOSYS;break;
        }
        out.len+=length;memcpy(reply,&out,sizeof(out));
        if(write(s->fd,reply,out.len)!=(ssize_t)out.len)break;
    }
    free(request);free(reply);return NULL;
}
JNIEXPORT jlong JNICALL Java_dev_kreijstal_aosppocket_storageprobe_MainActivity_startServer(JNIEnv* env,jclass klass,jint fd) {
    (void)env;(void)klass;struct Server* s=calloc(1,sizeof(*s));
    if(!s){close(fd);return 0;}s->fd=fd;memset(s->data,0x3d,sizeof(s->data));
    if(pthread_create(&s->thread,NULL,serve,s)){close(fd);free(s);return 0;}
    return (jlong)(intptr_t)s;
}
JNIEXPORT void JNICALL Java_dev_kreijstal_aosppocket_storageprobe_MainActivity_stopServer(JNIEnv* env,jclass klass,jlong handle) {
    (void)env;(void)klass;struct Server* s=(void*)(intptr_t)handle;
    shutdown(s->fd,SHUT_RDWR);pthread_join(s->thread,NULL);close(s->fd);free(s);
}
JNIEXPORT jstring JNICALL Java_dev_kreijstal_aosppocket_storageprobe_MainActivity_testDescriptors(JNIEnv* env,jclass klass,jint fd,jint second) {
    (void)klass;char json[1024];errno=0;
    void* read_map=mmap(NULL,4096,PROT_READ,MAP_PRIVATE,fd,0);int read_error=read_map==MAP_FAILED?errno:0;
    int content_ok=read_map!=MAP_FAILED && *(uint8_t*)read_map==0x3d;
    if(read_map!=MAP_FAILED)munmap(read_map,4096);
    errno=0;void* shared=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);int shared_error=shared==MAP_FAILED?errno:0,flush_error=-1;
    if(shared!=MAP_FAILED){((uint8_t*)shared)[1]=0x51;errno=0;flush_error=msync(shared,4096,MS_SYNC)<0?errno:0;munmap(shared,4096);}
    errno=0;void* exec=mmap(NULL,4096,PROT_READ|PROT_EXEC,MAP_PRIVATE,fd,0);int exec_error=exec==MAP_FAILED?errno:0;
    if(exec!=MAP_FAILED)munmap(exec,4096);
    errno=0;int truncate_error=ftruncate(fd,65536)<0?errno:0;
    struct flock lock={.l_type=F_WRLCK,.l_whence=SEEK_SET,.l_start=16384,.l_len=1};errno=0;
    int lock_error=fcntl(fd,F_SETLK,&lock)<0?errno:0,conflict=-1;
    if(!lock_error && second>=0) {
        pid_t child=fork();if(!child){int r=fcntl(second,F_SETLK,&lock);_exit(r<0 && (errno==EAGAIN || errno==EACCES)?0:1);}
        if(child>0){int status;if(waitpid(child,&status,0)==child)conflict=WIFEXITED(status)&&WEXITSTATUS(status)==0;}
    }
    lock.l_type=F_UNLCK;fcntl(fd,F_SETLK,&lock);
    snprintf(json,sizeof(json),"{\"readMmapErrno\":%d,\"initialContentMatches\":%s,\"sharedMmapErrno\":%d,\"msyncErrno\":%d,\"execMmapErrno\":%d,\"ftruncateErrno\":%d,\"fcntlLockErrno\":%d,\"crossProcessLockConflict\":%d}",read_error,content_ok?"true":"false",shared_error,flush_error,exec_error,truncate_error,lock_error,conflict);
    return (*env)->NewStringUTF(env,json);
}
