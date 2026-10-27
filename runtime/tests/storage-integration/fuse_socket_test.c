// SPDX-License-Identifier: GPL-3.0-only
#include <jni.h>
#include <linux/fuse.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>
#include <stdint.h>
JNIEXPORT jintArray JNICALL Java_dev_kreijstal_aosppocket_runtime_CeFuseConcurrencyTest_pair(JNIEnv* env,jclass type) {
    (void)type;int fds[2];if(socketpair(AF_UNIX,SOCK_SEQPACKET,0,fds))return NULL;
    jintArray out=(*env)->NewIntArray(env,2);(*env)->SetIntArrayRegion(env,out,0,2,fds);return out;
}
JNIEXPORT void JNICALL Java_dev_kreijstal_aosppocket_runtime_CeFuseConcurrencyTest_send(JNIEnv* env,jclass type,jint fd,jint opcode,jlong unique) {
    (void)env;(void)type;uint8_t bytes[256]={0};struct fuse_in_header in={.len=sizeof(in),.opcode=opcode,.unique=unique,.nodeid=3};
    if(opcode==FUSE_FSYNC){struct fuse_fsync_in arg={.fh=3};memcpy(bytes+sizeof(in),&arg,sizeof(arg));in.len+=sizeof(arg);}
    if(opcode==FUSE_WRITE){struct fuse_write_in arg={.fh=3,.size=4};memcpy(bytes+sizeof(in),&arg,sizeof(arg));in.len+=sizeof(arg)+4;}
    memcpy(bytes,&in,sizeof(in));(void)send(fd,bytes,in.len,MSG_NOSIGNAL);
}
JNIEXPORT jlongArray JNICALL Java_dev_kreijstal_aosppocket_runtime_CeFuseConcurrencyTest_receive(JNIEnv* env,jclass type,jint fd,jint timeout) {
    (void)type;struct pollfd p={.fd=fd,.events=POLLIN};jlong values[2]={-1,0};
    if(poll(&p,1,timeout)>0){uint8_t bytes[262144];ssize_t n=read(fd,bytes,sizeof(bytes));
        if(n>=(ssize_t)sizeof(struct fuse_out_header)){struct fuse_out_header out;memcpy(&out,bytes,sizeof(out));values[0]=out.unique;values[1]=out.error;}}
    jlongArray out=(*env)->NewLongArray(env,2);(*env)->SetLongArrayRegion(env,out,0,2,values);return out;
}
JNIEXPORT void JNICALL Java_dev_kreijstal_aosppocket_runtime_CeFuseConcurrencyTest_closeFd(JNIEnv* env,jclass type,jint fd) {(void)env;(void)type;close(fd);}
