// SPDX-License-Identifier: GPL-3.0-only
#include <jni.h>
#include <errno.h>
#include "uevent.h"

extern "C" JNIEXPORT jint JNICALL
Java_dev_kreijstal_aosppocket_guest_UEventProbe_inject(JNIEnv* env,jclass,jbyteArray bytes) {
    if(!bytes)return -EINVAL;
    const jsize length=env->GetArrayLength(bytes);
    if(length<1 || length>POCKET_UEVENT_MAX32)return -EINVAL;
    char record[POCKET_UEVENT_MAX32];
    env->GetByteArrayRegion(bytes,0,length,reinterpret_cast<jbyte*>(record));
    if(env->ExceptionCheck())return -EFAULT;
    const int result=pocket_uevent_inject32(record,length);
    return result<0?-errno:result;
}
