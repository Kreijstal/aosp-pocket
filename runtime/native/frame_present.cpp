// SPDX-License-Identifier: GPL-3.0-only
#include <jni.h>
#include <android/native_window_jni.h>
#include <cstdint>
#include <cstring>

// Software-composited guest pixels go straight into a host Surface buffer.
// No image codec, Java Bitmap, Canvas or host UI-thread draw per frame.
extern "C" JNIEXPORT jboolean JNICALL
Java_dev_kreijstal_aosppocket_runtime_Native_presentFrame(
    JNIEnv* env, jclass, jobject surface, jobject pixels, jint width, jint height) {
    auto* source=static_cast<uint8_t*>(env->GetDirectBufferAddress(pixels));
    const jlong size=env->GetDirectBufferCapacity(pixels);
    if(!surface || !source || width<1 || height<1 || width>8192 || height>8192 ||
       int64_t(width)*height>16*1024*1024 || size<int64_t(width)*height*4)return JNI_FALSE;
    ANativeWindow* window=ANativeWindow_fromSurface(env,surface);
    if(!window)return JNI_FALSE;
    if(ANativeWindow_getWidth(window)!=width || ANativeWindow_getHeight(window)!=height ||
       ANativeWindow_getFormat(window)!=WINDOW_FORMAT_RGBA_8888) {
        if(ANativeWindow_setBuffersGeometry(window,width,height,WINDOW_FORMAT_RGBA_8888)!=0) {
            ANativeWindow_release(window);return JNI_FALSE;
        }
    }
    ANativeWindow_Buffer buffer{};
    if(ANativeWindow_lock(window,&buffer,nullptr)!=0) {ANativeWindow_release(window);return JNI_FALSE;}
    bool valid=buffer.bits && buffer.width==width && buffer.height==height &&
        buffer.stride>=width && buffer.format==WINDOW_FORMAT_RGBA_8888;
    if(valid) {
        auto* target=static_cast<uint8_t*>(buffer.bits);
        for(int row=0;row<height;row++)
            std::memcpy(target+size_t(row)*buffer.stride*4,source+size_t(row)*width*4,size_t(width)*4);
    }
    int posted=ANativeWindow_unlockAndPost(window);
    ANativeWindow_release(window);
    return valid && posted==0 ? JNI_TRUE : JNI_FALSE;
}
