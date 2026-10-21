// SPDX-License-Identifier: GPL-3.0-only
#include <jni.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static long direct(long number, long a, long b, long c) {
    register long x0 __asm__("x0")=a;
    register long x1 __asm__("x1")=b;
    register long x2 __asm__("x2")=c;
    register long x3 __asm__("x3")=0;
    register long x8 __asm__("x8")=number;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1),"r"(x2),"r"(x3),"r"(x8) : "memory","cc");
    return x0;
}
static int correct(int fd) {
    char text[32]={0};
    ssize_t count=read(fd,text,sizeof(text));
    close(fd);
    return count==14 && !memcmp(text,"pocket-openat\n",14);
}
JNIEXPORT jstring JNICALL Java_dev_kreijstal_pocketsyscalltest_MainActivity_probe(JNIEnv *env,jclass type,jstring name) {
    (void)type;
    const char *path=(*env)->GetStringUTFChars(env,name,NULL);
    if(!path)return NULL;
    int libc_fd=openat(AT_FDCWD,path,O_RDONLY|O_CLOEXEC);
    int libc_error=libc_fd<0?errno:0;
    int libc_ok=libc_fd>=0 && correct(libc_fd);
    long raw_fd=direct(__NR_openat,AT_FDCWD,(long)path,O_RDONLY|O_CLOEXEC);
    int raw_error=raw_fd<0?(int)-raw_fd:0;
    int raw_ok=raw_fd>=0 && correct((int)raw_fd);
    (*env)->ReleaseStringUTFChars(env,name,path);
    char report[256];
    snprintf(report,sizeof(report),"{\"libcOk\":%s,\"directOk\":%s,\"libcErrno\":%d,\"directErrno\":%d,\"kernelUid\":%ld}",
             libc_ok?"true":"false",raw_ok?"true":"false",libc_error,raw_error,direct(__NR_getuid,0,0,0));
    return (*env)->NewStringUTF(env,report);
}
