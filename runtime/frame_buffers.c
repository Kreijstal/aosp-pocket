// SPDX-License-Identifier: GPL-3.0-only
#include <jni.h>
#include <android/sharedmem.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail(JNIEnv* env,const char* reason) {
    (*env)->ThrowNew(env,(*env)->FindClass(env,"java/io/IOException"),reason);
}
JNIEXPORT jint JNICALL Java_dev_kreijstal_aosppocket_bridge_AnonymousFrames_createRegion(JNIEnv* env,jclass type,jint bytes) {
    (void)type;
    if(bytes<8 || bytes>128*1024*1024){fail(env,"Invalid shared display size");return -1;}
    int fd=ASharedMemory_create("pocket-display",bytes);
    if(fd<0)fail(env,"Cannot allocate anonymous display memory");
    return fd;
}
JNIEXPORT jobject JNICALL Java_dev_kreijstal_aosppocket_bridge_AnonymousFrames_mapRegion(JNIEnv* env,jclass type,jobject descriptor,jint bytes) {
    (void)type;
    if(bytes<8 || bytes>128*1024*1024 || !descriptor){fail(env,"Invalid shared display mapping");return NULL;}
    jfieldID field=(*env)->GetFieldID(env,(*env)->GetObjectClass(env,descriptor),"descriptor","I");
    if(!field)return NULL;
    int fd=(*env)->GetIntField(env,descriptor,field);struct stat status;
    // Modern host memfds work even with Android 9's older ashmem helper.
    if(fstat(fd,&status)!=0 || (status.st_size!=bytes && ASharedMemory_getSize(fd)!=(size_t)bytes)) {
        fail(env,"Shared display descriptor size does not match geometry");return NULL;
    }
    void* memory=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(memory==MAP_FAILED){fail(env,"Cannot map shared display memory");return NULL;}
    jobject result=(*env)->NewDirectByteBuffer(env,memory,bytes);
    if(!result)munmap(memory,bytes);
    return result;
}
JNIEXPORT void JNICALL Java_dev_kreijstal_aosppocket_bridge_AnonymousFrames_unmapRegion(JNIEnv* env,jclass type,jobject memory) {
    (void)type;
    void* address=(*env)->GetDirectBufferAddress(env,memory);
    jlong size=(*env)->GetDirectBufferCapacity(env,memory);
    if(address && size>0)munmap(address,size);
}
