// SPDX-License-Identifier: GPL-3.0-only
#define _GNU_SOURCE
#include <jni.h>
#include <linux/fuse.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define SYNC_QUEUE 64
struct Server {
    int fd,stopping;pthread_t thread,sync_thread;
    pthread_mutex_t replies,work;pthread_cond_t available,space;
    uint64_t sync_ids[SYNC_QUEUE];unsigned head,count;
    JavaVM* vm;jobject callback;jmethodID size,read,write,sync,opened,released;
};
static int attach(struct Server* s,JNIEnv** env) {
#ifdef __ANDROID__
    return (*s->vm)->AttachCurrentThread(s->vm,env,NULL);
#else
    return (*s->vm)->AttachCurrentThread(s->vm,(void**)env,NULL);
#endif
}
static void stopping(struct Server* s) {
    pthread_mutex_lock(&s->work);s->stopping=1;
    pthread_cond_broadcast(&s->available);pthread_cond_broadcast(&s->space);pthread_mutex_unlock(&s->work);
}
static int respond(struct Server* s,const void* data,size_t size) {
    // FUSE replies may complete out of order, but each unique-ID frame must
    // remain intact on either the AppFuse socket or a direct FUSE descriptor.
    pthread_mutex_lock(&s->replies);ssize_t n;
    do {
        n=send(s->fd,data,size,MSG_NOSIGNAL);
        if(n<0 && errno==ENOTSOCK)n=write(s->fd,data,size);
    }while(n<0 && errno==EINTR);
    pthread_mutex_unlock(&s->replies);
    if(n!=(ssize_t)size){stopping(s);shutdown(s->fd,SHUT_RDWR);return -1;}
    return 0;
}
static int enqueue_sync(struct Server* s,uint64_t id) {
    pthread_mutex_lock(&s->work);
    while(s->count==SYNC_QUEUE && !s->stopping)pthread_cond_wait(&s->space,&s->work);
    if(s->stopping){pthread_mutex_unlock(&s->work);return -1;}
    s->sync_ids[(s->head+s->count)%SYNC_QUEUE]=id;s->count++;
    pthread_cond_signal(&s->available);pthread_mutex_unlock(&s->work);return 0;
}
static void* sync_requests(void* opaque) {
    struct Server* s=opaque;JNIEnv* env=NULL;
    if(attach(s,&env)!=JNI_OK){stopping(s);shutdown(s->fd,SHUT_RDWR);return NULL;}
    for(;;) {
        pthread_mutex_lock(&s->work);
        while(!s->count && !s->stopping)pthread_cond_wait(&s->available,&s->work);
        if(!s->count && s->stopping){pthread_mutex_unlock(&s->work);break;}
        uint64_t id=s->sync_ids[s->head];s->head=(s->head+1)%SYNC_QUEUE;s->count--;
        pthread_cond_signal(&s->space);pthread_mutex_unlock(&s->work);
        struct fuse_out_header out={.len=sizeof(out),.unique=id};
        (*env)->CallVoidMethod(env,s->callback,s->sync);
        if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);out.error=-EIO;}
        respond(s,&out,sizeof(out));
    }
    (*s->vm)->DetachCurrentThread(s->vm);return NULL;
}
static int attrs(JNIEnv* env,struct Server* s,struct fuse_attr* a,uint64_t inode) {
    memset(a,0,sizeof(*a));a->ino=inode;a->mode=inode==FUSE_ROOT_ID?(S_IFDIR|0700):(S_IFREG|0600);
    a->nlink=1;a->uid=getuid();a->gid=getgid();a->blksize=4096;
    if(inode>2){jlong size=(*env)->CallLongMethod(env,s->callback,s->size,(jint)inode);if((*env)->ExceptionCheck(env))return -EIO;if(size<0)return -ENOENT;a->size=size;}
    a->blocks=(a->size+511)/512;return 0;
}
static void* serve(void* opaque) {
    struct Server* s=opaque;JNIEnv* env=NULL;
    if(attach(s,&env)!=JNI_OK){stopping(s);shutdown(s->fd,SHUT_RDWR);return NULL;}
    uint8_t* request=malloc(262144),*reply=malloc(262144);
    if(!request || !reply)goto done;
    for(;;) {
        ssize_t n=read(s->fd,request,262144);if(n<0 && errno==EINTR)continue;
        if(n<(ssize_t)sizeof(struct fuse_in_header))break;
        struct fuse_in_header in;memcpy(&in,request,sizeof(in));if(in.len!=(uint32_t)n)break;
        if(in.opcode==FUSE_FORGET || in.opcode==FUSE_BATCH_FORGET)continue;
        struct fuse_out_header out={.len=sizeof(out),.unique=in.unique};size_t length=0;
        void* body=reply+sizeof(out);uint8_t* args=request+sizeof(in);
        switch(in.opcode) {
        case FUSE_LOOKUP: {
            if(n<=(ssize_t)sizeof(in) || args[n-sizeof(in)-1]!=0){out.error=-EINVAL;break;}
            char* end=NULL;unsigned long node=strtoul((char*)args,&end,10);
            if(!node || node>INT32_MAX || !end || *end){out.error=-ENOENT;break;}
            struct fuse_entry_out value={.nodeid=node,.generation=1};out.error=attrs(env,s,&value.attr,node);
            if(!out.error){length=sizeof(value);memcpy(body,&value,length);}break;
        }
        case FUSE_GETATTR: {
            struct fuse_attr_out value={0};out.error=attrs(env,s,&value.attr,in.nodeid);
            if(!out.error){length=sizeof(value);memcpy(body,&value,length);}break;
        }
        case FUSE_OPEN: {
            if(in.nodeid>2)(*env)->CallVoidMethod(env,s->callback,s->opened,(jint)in.nodeid);
            struct fuse_open_out value={.fh=in.nodeid,.open_flags=FOPEN_KEEP_CACHE};length=sizeof(value);memcpy(body,&value,length);break;
        }
        case FUSE_READ: {
            if(n<(ssize_t)(sizeof(in)+sizeof(struct fuse_read_in))){out.error=-EINVAL;break;}
            struct fuse_read_in value;memcpy(&value,args,sizeof(value));if(value.size>131072){out.error=-EINVAL;break;}
            jbyteArray bytes=(*env)->CallObjectMethod(env,s->callback,s->read,(jint)in.nodeid,(jlong)value.offset,(jint)value.size);
            if(!bytes || (*env)->ExceptionCheck(env)){out.error=-EIO;break;}
            length=(*env)->GetArrayLength(env,bytes);if(length>value.size){out.error=-EIO;length=0;}
            if(length)(*env)->GetByteArrayRegion(env,bytes,0,length,body);
            (*env)->DeleteLocalRef(env,bytes);break;
        }
        case FUSE_WRITE: {
            if(n<(ssize_t)(sizeof(in)+sizeof(struct fuse_write_in))){out.error=-EINVAL;break;}
            struct fuse_write_in value;memcpy(&value,args,sizeof(value));
            if(value.size>131072 || value.size>(size_t)n-sizeof(in)-sizeof(value)){out.error=-EINVAL;break;}
            jbyteArray bytes=(*env)->NewByteArray(env,value.size);if(!bytes){out.error=-ENOMEM;break;}
            (*env)->SetByteArrayRegion(env,bytes,0,value.size,(jbyte*)(args+sizeof(value)));
            jint written=(*env)->CallIntMethod(env,s->callback,s->write,(jint)in.nodeid,(jlong)value.offset,bytes);(*env)->DeleteLocalRef(env,bytes);
            if(written<0 || (*env)->ExceptionCheck(env)){out.error=-EIO;break;}
            struct fuse_write_out result={.size=(uint32_t)written};length=sizeof(result);memcpy(body,&result,length);break;
        }
        case FUSE_FSYNC:
            if(n<(ssize_t)(sizeof(in)+sizeof(struct fuse_fsync_in))){out.error=-EINVAL;break;}
            // All earlier WRITE callbacks have finished. The worker captures
            // their state and replies only after durable sync, while unrelated
            // reads/attributes keep flowing through this receive loop.
            if(enqueue_sync(s,in.unique)<0)goto done;
            continue;
        case FUSE_RELEASE:if(in.nodeid>2)(*env)->CallVoidMethod(env,s->callback,s->released,(jint)in.nodeid);break;
        case FUSE_FLUSH:break;
        default:out.error=-ENOSYS;break;
        }
        if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);out.error=-EIO;length=0;}
        out.len+=length;memcpy(reply,&out,sizeof(out));if(respond(s,reply,out.len)<0)break;
    }
done:stopping(s);shutdown(s->fd,SHUT_RDWR);free(request);free(reply);(*s->vm)->DetachCurrentThread(s->vm);return NULL;
}
static void destroy(struct Server* s) {
    pthread_cond_destroy(&s->space);pthread_cond_destroy(&s->available);
    pthread_mutex_destroy(&s->work);pthread_mutex_destroy(&s->replies);free(s);
}
JNIEXPORT jlong JNICALL Java_dev_kreijstal_aosppocket_runtime_CeFuse_start(JNIEnv* env,jclass type,jint fd,jobject callback) {
    (void)type;struct Server* s=calloc(1,sizeof(*s));if(!s){close(fd);return 0;}s->fd=fd;
    if(pthread_mutex_init(&s->replies,NULL)){close(fd);free(s);return 0;}
    if(pthread_mutex_init(&s->work,NULL)){pthread_mutex_destroy(&s->replies);close(fd);free(s);return 0;}
    if(pthread_cond_init(&s->available,NULL)){pthread_mutex_destroy(&s->work);pthread_mutex_destroy(&s->replies);close(fd);free(s);return 0;}
    if(pthread_cond_init(&s->space,NULL)){pthread_cond_destroy(&s->available);pthread_mutex_destroy(&s->work);pthread_mutex_destroy(&s->replies);close(fd);free(s);return 0;}
    (*env)->GetJavaVM(env,&s->vm);s->callback=(*env)->NewGlobalRef(env,callback);jclass c=(*env)->GetObjectClass(env,callback);
    s->size=(*env)->GetMethodID(env,c,"fuseSize","(I)J");s->read=(*env)->GetMethodID(env,c,"fuseRead","(IJI)[B");
    s->write=(*env)->GetMethodID(env,c,"fuseWrite","(IJ[B)I");s->sync=(*env)->GetMethodID(env,c,"fuseSync","()V");s->opened=(*env)->GetMethodID(env,c,"fuseOpened","(I)V");s->released=(*env)->GetMethodID(env,c,"fuseRelease","(I)V");
    if((*env)->ExceptionCheck(env) || !s->callback || pthread_create(&s->sync_thread,NULL,sync_requests,s)) {
        if(s->callback)(*env)->DeleteGlobalRef(env,s->callback);
        close(fd);destroy(s);return 0;
    }
    if(pthread_create(&s->thread,NULL,serve,s)) {
        stopping(s);shutdown(fd,SHUT_RDWR);pthread_join(s->sync_thread,NULL);
        (*env)->DeleteGlobalRef(env,s->callback);close(fd);destroy(s);return 0;
    }
    return (jlong)(intptr_t)s;
}
JNIEXPORT void JNICALL Java_dev_kreijstal_aosppocket_runtime_CeFuse_stop(JNIEnv* env,jclass type,jlong handle) {
    (void)type;struct Server* s=(void*)(intptr_t)handle;if(!s)return;
    stopping(s);shutdown(s->fd,SHUT_RDWR);
    pthread_join(s->thread,NULL);pthread_join(s->sync_thread,NULL);
    close(s->fd);(*env)->DeleteGlobalRef(env,s->callback);destroy(s);
}
