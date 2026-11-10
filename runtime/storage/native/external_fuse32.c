// SPDX-License-Identifier: GPL-3.0-only
// API32 external-file AppFuse bridge. Callbacks preserve kernel caller tid,
// per-open handles, errno and fsync flags; legacy CE JNI remains independent.
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
#include "external_holders32.h"
#define SYNC_QUEUE 64
struct SyncRequest {uint64_t unique,node,handle;uint32_t pid,flags;};
struct Server {
    int fd,stopping;pthread_t thread,sync_thread;
    pthread_mutex_t replies,work;pthread_cond_t available,space;
    struct SyncRequest sync_ids[SYNC_QUEUE];unsigned head,count;
    JavaVM* vm;jobject callback;jclass errno_class;jfieldID errno_field;
    jmethodID size,read,write,sync,opened,released,flush;
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
static int callback_error(JNIEnv* env,struct Server* s) {
    jthrowable error=(*env)->ExceptionOccurred(env);if(!error)return 0;
    (*env)->ExceptionClear(env);int code=EIO;
    if((*env)->IsInstanceOf(env,error,s->errno_class))code=(*env)->GetIntField(env,error,s->errno_field);
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);code=EIO;}
    (*env)->DeleteLocalRef(env,error);return code>0 && code<=4095?-code:-EIO;
}
static int enqueue_sync(struct Server* s,struct SyncRequest request) {
    pthread_mutex_lock(&s->work);
    while(s->count==SYNC_QUEUE && !s->stopping)pthread_cond_wait(&s->space,&s->work);
    if(s->stopping){pthread_mutex_unlock(&s->work);return -1;}
    s->sync_ids[(s->head+s->count)%SYNC_QUEUE]=request;s->count++;
    pthread_cond_signal(&s->available);pthread_mutex_unlock(&s->work);return 0;
}
static void* sync_requests(void* opaque) {
    struct Server* s=opaque;JNIEnv* env=NULL;
    if(attach(s,&env)!=JNI_OK){stopping(s);shutdown(s->fd,SHUT_RDWR);return NULL;}
    for(;;) {
        pthread_mutex_lock(&s->work);
        while(!s->count && !s->stopping)pthread_cond_wait(&s->available,&s->work);
        if(!s->count && s->stopping){pthread_mutex_unlock(&s->work);break;}
        struct SyncRequest request=s->sync_ids[s->head];s->head=(s->head+1)%SYNC_QUEUE;s->count--;
        pthread_cond_signal(&s->space);pthread_mutex_unlock(&s->work);
        struct fuse_out_header out={.len=sizeof(out),.unique=request.unique};
        (*env)->CallVoidMethod(env,s->callback,s->sync,(jint)request.node,(jlong)request.handle,(jint)request.pid,(jint)request.flags);
        out.error=callback_error(env,s);
        respond(s,&out,sizeof(out));
    }
    (*s->vm)->DetachCurrentThread(s->vm);return NULL;
}
static int attrs(JNIEnv* env,struct Server* s,struct fuse_attr* a,uint64_t inode,uint32_t pid) {
    memset(a,0,sizeof(*a));a->ino=inode;a->mode=inode==FUSE_ROOT_ID?(S_IFDIR|0700):(S_IFREG|0600);
    a->nlink=1;a->uid=getuid();a->gid=getgid();a->blksize=4096;
    if(inode>2){jlong size=(*env)->CallLongMethod(env,s->callback,s->size,(jint)inode,(jint)pid);if((*env)->ExceptionCheck(env))return callback_error(env,s);if(size<0)return size>=-4095?(int)size:-EIO;a->size=size;}
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
        if(in.nodeid>INT32_MAX || in.pid>INT32_MAX){out.error=-EINVAL;memcpy(reply,&out,sizeof(out));if(respond(s,reply,sizeof(out))<0)break;continue;}
        void* body=reply+sizeof(out);uint8_t* args=request+sizeof(in);
        switch(in.opcode) {
        case FUSE_LOOKUP: {
            if(n<=(ssize_t)sizeof(in) || args[n-sizeof(in)-1]!=0){out.error=-EINVAL;break;}
            char* end=NULL;unsigned long node=strtoul((char*)args,&end,10);
            if(!node || node>INT32_MAX || !end || *end){out.error=-ENOENT;break;}
            struct fuse_entry_out value={.nodeid=node,.generation=1};out.error=attrs(env,s,&value.attr,node,in.pid);
            if(!out.error){length=sizeof(value);memcpy(body,&value,length);}break;
        }
        case FUSE_GETATTR: {
            struct fuse_attr_out value={0};out.error=attrs(env,s,&value.attr,in.nodeid,in.pid);
            if(!out.error){length=sizeof(value);memcpy(body,&value,length);}break;
        }
        case FUSE_OPEN: {
            if(n<(ssize_t)(sizeof(in)+sizeof(struct fuse_open_in))){out.error=-EINVAL;break;}
            struct fuse_open_in input;memcpy(&input,args,sizeof(input));
            struct fuse_open_out value={.fh=in.nodeid,.open_flags=FOPEN_KEEP_CACHE};
            if(in.nodeid>2) {
                jlongArray opened=(*env)->CallObjectMethod(env,s->callback,s->opened,(jint)in.nodeid,(jint)in.pid,(jint)input.flags);
                if(!opened || (*env)->ExceptionCheck(env)){out.error=callback_error(env,s);if(!out.error)out.error=-EIO;if(opened)(*env)->DeleteLocalRef(env,opened);break;}
                if((*env)->GetArrayLength(env,opened)!=2){(*env)->DeleteLocalRef(env,opened);out.error=-EIO;break;}
                jlong fields[2];(*env)->GetLongArrayRegion(env,opened,0,2,fields);(*env)->DeleteLocalRef(env,opened);
                if(fields[1]<0 || (fields[1]&~(jlong)(FOPEN_DIRECT_IO|FOPEN_KEEP_CACHE))){out.error=-EINVAL;break;}
                value.fh=(uint64_t)fields[0];value.open_flags=(uint32_t)fields[1];
            }
            length=sizeof(value);memcpy(body,&value,length);break;
        }
        case FUSE_READ: {
            if(n<(ssize_t)(sizeof(in)+sizeof(struct fuse_read_in))){out.error=-EINVAL;break;}
            struct fuse_read_in value;memcpy(&value,args,sizeof(value));if(value.size>131072){out.error=-EINVAL;break;}
            jbyteArray bytes=(*env)->CallObjectMethod(env,s->callback,s->read,(jint)in.nodeid,(jlong)value.fh,(jlong)value.offset,(jint)value.size,(jint)in.pid);
            if(!bytes || (*env)->ExceptionCheck(env)){out.error=callback_error(env,s);if(!out.error)out.error=-EIO;if(bytes)(*env)->DeleteLocalRef(env,bytes);break;}
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
            jint written=(*env)->CallIntMethod(env,s->callback,s->write,(jint)in.nodeid,(jlong)value.fh,(jlong)value.offset,bytes,(jint)in.pid);(*env)->DeleteLocalRef(env,bytes);
            if((*env)->ExceptionCheck(env)){out.error=callback_error(env,s);break;}
            if(written<0){out.error=written>=-4095?written:-EIO;break;}
            if((uint32_t)written>value.size){out.error=-EIO;break;}
            struct fuse_write_out result={.size=(uint32_t)written};length=sizeof(result);memcpy(body,&result,length);break;
        }
        case FUSE_FSYNC: {
            if(n<(ssize_t)(sizeof(in)+sizeof(struct fuse_fsync_in))){out.error=-EINVAL;break;}
            struct fuse_fsync_in value;memcpy(&value,args,sizeof(value));
            struct SyncRequest sync={in.unique,in.nodeid,value.fh,in.pid,value.fsync_flags};
            if(enqueue_sync(s,sync)<0)goto done;
            continue;
        }
        case FUSE_RELEASE: {
            if(n<(ssize_t)(sizeof(in)+sizeof(struct fuse_release_in))){out.error=-EINVAL;break;}
            struct fuse_release_in value;memcpy(&value,args,sizeof(value));
            if(in.nodeid>2)(*env)->CallVoidMethod(env,s->callback,s->released,(jint)in.nodeid,(jlong)value.fh,(jint)in.pid,(jint)value.flags);
            break;
        }
        case FUSE_FLUSH: {
            if(n<(ssize_t)(sizeof(in)+sizeof(struct fuse_flush_in))){out.error=-EINVAL;break;}
            struct fuse_flush_in value;memcpy(&value,args,sizeof(value));
            if(in.nodeid>2)(*env)->CallVoidMethod(env,s->callback,s->flush,(jint)in.nodeid,(jlong)value.fh,(jint)in.pid);
            break;
        }
        default:out.error=-ENOSYS;break;
        }
        if((*env)->ExceptionCheck(env)){out.error=callback_error(env,s);length=0;}
        out.len+=length;memcpy(reply,&out,sizeof(out));if(respond(s,reply,out.len)<0)break;
    }
done:stopping(s);shutdown(s->fd,SHUT_RDWR);free(request);free(reply);(*s->vm)->DetachCurrentThread(s->vm);return NULL;
}
static void destroy(struct Server* s) {
    pthread_cond_destroy(&s->space);pthread_cond_destroy(&s->available);
    pthread_mutex_destroy(&s->work);pthread_mutex_destroy(&s->replies);free(s);
}
JNIEXPORT jlong JNICALL Java_dev_kreijstal_aosppocket_runtime_ExternalFuse32_start(JNIEnv* env,jclass type,jint fd,jobject callback) {
    (void)type;struct Server* s=calloc(1,sizeof(*s));if(!s){close(fd);return 0;}s->fd=fd;
    if(pthread_mutex_init(&s->replies,NULL)){close(fd);free(s);return 0;}
    if(pthread_mutex_init(&s->work,NULL)){pthread_mutex_destroy(&s->replies);close(fd);free(s);return 0;}
    if(pthread_cond_init(&s->available,NULL)){pthread_mutex_destroy(&s->work);pthread_mutex_destroy(&s->replies);close(fd);free(s);return 0;}
    if(pthread_cond_init(&s->space,NULL)){pthread_cond_destroy(&s->available);pthread_mutex_destroy(&s->work);pthread_mutex_destroy(&s->replies);close(fd);free(s);return 0;}
    jclass c=NULL,error=NULL;
    if(!callback || (*env)->GetJavaVM(env,&s->vm)!=JNI_OK)goto failed;
    s->callback=(*env)->NewGlobalRef(env,callback);if(!s->callback)goto failed;
    c=(*env)->GetObjectClass(env,callback);if(!c)goto failed;
    error=(*env)->FindClass(env,"android/system/ErrnoException");if(!error)goto failed;
    s->errno_class=(jclass)(*env)->NewGlobalRef(env,error);if(!s->errno_class)goto failed;
    s->errno_field=(*env)->GetFieldID(env,s->errno_class,"errno","I");if(!s->errno_field)goto failed;
#define METHOD(field,name,signature) s->field=(*env)->GetMethodID(env,c,name,signature);if(!s->field)goto failed
    METHOD(size,"fuseSize","(II)J");METHOD(read,"fuseRead","(IJJII)[B");
    METHOD(write,"fuseWrite","(IJJ[BI)I");METHOD(sync,"fuseSync","(IJII)V");
    METHOD(opened,"fuseOpen","(III)[J");METHOD(released,"fuseRelease","(IJII)V");
    METHOD(flush,"fuseFlush","(IJI)V");
#undef METHOD
    (*env)->DeleteLocalRef(env,c);c=NULL;(*env)->DeleteLocalRef(env,error);error=NULL;
    if(pthread_create(&s->sync_thread,NULL,sync_requests,s))goto failed;
    if(pthread_create(&s->thread,NULL,serve,s)) {
        stopping(s);shutdown(fd,SHUT_RDWR);pthread_join(s->sync_thread,NULL);
        (*env)->DeleteGlobalRef(env,s->callback);(*env)->DeleteGlobalRef(env,s->errno_class);close(fd);destroy(s);return 0;
    }
    return (jlong)(intptr_t)s;
failed:
    if(c)(*env)->DeleteLocalRef(env,c);
    if(error)(*env)->DeleteLocalRef(env,error);
    if(s->callback)(*env)->DeleteGlobalRef(env,s->callback);
    if(s->errno_class)(*env)->DeleteGlobalRef(env,s->errno_class);
    close(fd);destroy(s);return 0;
}
JNIEXPORT void JNICALL Java_dev_kreijstal_aosppocket_runtime_ExternalFuse32_stop(JNIEnv* env,jclass type,jlong handle) {
    (void)type;struct Server* s=(void*)(intptr_t)handle;if(!s)return;
    stopping(s);shutdown(s->fd,SHUT_RDWR);
    pthread_join(s->thread,NULL);pthread_join(s->sync_thread,NULL);
    close(s->fd);(*env)->DeleteGlobalRef(env,s->callback);(*env)->DeleteGlobalRef(env,s->errno_class);destroy(s);
}
JNIEXPORT jintArray JNICALL Java_dev_kreijstal_aosppocket_runtime_ExternalFuse32_retireHolders(JNIEnv* env,jclass type,jstring root,jlong device,jboolean cached_stat) {
    (void)type;if(!root || device==0)return NULL;
    const char* path=(*env)->GetStringUTFChars(env,root,NULL);if(!path)return NULL;
    pid_t retired[4096];int count=pocket_retire_holders32(path,(dev_t)device,retired,4096,cached_stat==JNI_TRUE);(*env)->ReleaseStringUTFChars(env,root,path);
    if(count<0) {
        jclass error=(*env)->FindClass(env,"android/system/ErrnoException");if(!error)return NULL;
        jmethodID ctor=(*env)->GetMethodID(env,error,"<init>","(Ljava/lang/String;I)V");if(!ctor){(*env)->DeleteLocalRef(env,error);return NULL;}
        jstring name=(*env)->NewStringUTF(env,"retire-external-volume-holders");
        if(name){jthrowable problem=(*env)->NewObject(env,error,ctor,name,(jint)-count);if(problem){(*env)->Throw(env,problem);(*env)->DeleteLocalRef(env,problem);}(*env)->DeleteLocalRef(env,name);}
        (*env)->DeleteLocalRef(env,error);return NULL;
    }
    jintArray result=(*env)->NewIntArray(env,count);if(result && count)(*env)->SetIntArrayRegion(env,result,0,count,(const jint*)retired);return result;
}
JNIEXPORT jint JNICALL Java_dev_kreijstal_aosppocket_runtime_ExternalFuse32_probeRetirement(JNIEnv* env,jclass type) {
    (void)env;(void)type;
#if defined(__NR_pidfd_open) && defined(__NR_pidfd_send_signal)
    int fd=(int)syscall(__NR_pidfd_open,getpid(),0);if(fd<0)return -errno;
    int result=syscall(__NR_pidfd_send_signal,fd,0,NULL,0)?-errno:0;close(fd);return result;
#else
    return -ENOSYS;
#endif
}
