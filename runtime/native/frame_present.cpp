// SPDX-License-Identifier: GPL-3.0-only
#include <jni.h>
#include <android/hardware_buffer.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <poll.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <cstdio>
#include <cerrno>

namespace {
struct Presenter {
    ANativeWindow* window=nullptr;
    EGLDisplay display=EGL_NO_DISPLAY;
    EGLContext context=EGL_NO_CONTEXT;
    EGLConfig config=nullptr;
    EGLSurface output=EGL_NO_SURFACE,scratch=EGL_NO_SURFACE;
    AHardwareBuffer* buffers[2]{};
    EGLImageKHR images[2]{EGL_NO_IMAGE_KHR,EGL_NO_IMAGE_KHR};
    EGLSyncKHR fences[2]{EGL_NO_SYNC_KHR,EGL_NO_SYNC_KHR};
    GLuint textures[2]{},program=0;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC clientBuffer=nullptr;
    PFNEGLCREATEIMAGEKHRPROC createImage=nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage=nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC targetImage=nullptr;
    PFNEGLCREATESYNCKHRPROC createSync=nullptr;
    PFNEGLDESTROYSYNCKHRPROC destroySync=nullptr;
    PFNEGLCLIENTWAITSYNCKHRPROC waitSync=nullptr;
    int width=0,height=0,next=0,failedFrames=0;
    bool attempted=false,gpu=false;
    std::string backend="uninitialized",renderer,vendor,reason;
    ~Presenter(){clearGL();if(window)ANativeWindow_release(window);}
    void failure(const char* what) {
        char code[32];std::snprintf(code,sizeof(code)," (EGL 0x%x)",eglGetError());reason=what;reason+=code;
    }
    void clearWindow() {
        if(display!=EGL_NO_DISPLAY && context!=EGL_NO_CONTEXT && scratch!=EGL_NO_SURFACE)
            eglMakeCurrent(display,scratch,scratch,context);
        if(output!=EGL_NO_SURFACE){eglDestroySurface(display,output);output=EGL_NO_SURFACE;}
    }
    void clearGL() {
        clearWindow();
        if(context!=EGL_NO_CONTEXT && scratch!=EGL_NO_SURFACE && eglMakeCurrent(display,scratch,scratch,context)) {
            glFinish();
            for(int i=0;i<2;i++) {
                if(fences[i]!=EGL_NO_SYNC_KHR && destroySync)destroySync(display,fences[i]);fences[i]=EGL_NO_SYNC_KHR;
                if(textures[i])glDeleteTextures(1,&textures[i]);textures[i]=0;
            }
            if(program)glDeleteProgram(program);program=0;
        }
        for(int i=0;i<2;i++) {
            if(images[i]!=EGL_NO_IMAGE_KHR && destroyImage)destroyImage(display,images[i]);images[i]=EGL_NO_IMAGE_KHR;
            if(buffers[i])AHardwareBuffer_release(buffers[i]);buffers[i]=nullptr;
        }
        if(display!=EGL_NO_DISPLAY) {
            eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
            if(scratch!=EGL_NO_SURFACE)eglDestroySurface(display,scratch);
            if(context!=EGL_NO_CONTEXT)eglDestroyContext(display,context);
            // EGL_DEFAULT_DISPLAY is also used by the legacy guest renderer.
            // Do not terminate its process-wide display during a session close.
        }
        scratch=EGL_NO_SURFACE;context=EGL_NO_CONTEXT;gpu=false;
    }
    GLuint shader(GLenum kind,const char* source) {
        GLuint result=glCreateShader(kind);glShaderSource(result,1,&source,nullptr);glCompileShader(result);
        GLint compiled=0;glGetShaderiv(result,GL_COMPILE_STATUS,&compiled);
        if(!compiled){char log[512]{};glGetShaderInfoLog(result,sizeof(log),nullptr,log);reason=log;glDeleteShader(result);return 0;}return result;
    }
    bool initialize(int w,int h) {
        attempted=true;width=w;height=h;
        display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if(display==EGL_NO_DISPLAY || !eglInitialize(display,nullptr,nullptr)){failure("Cannot initialize host EGL");return false;}
        EGLint options[]={EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,
            EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_SURFACE_TYPE,EGL_WINDOW_BIT|EGL_PBUFFER_BIT,EGL_NONE};
        EGLint count=0;if(!eglChooseConfig(display,options,&config,1,&count)||!count){failure("No RGBA host EGL config");return false;}
        EGLint version[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
        context=eglCreateContext(display,config,EGL_NO_CONTEXT,version);
        EGLint tiny[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};scratch=eglCreatePbufferSurface(display,config,tiny);
        if(context==EGL_NO_CONTEXT || scratch==EGL_NO_SURFACE || !eglMakeCurrent(display,scratch,scratch,context)) {
            failure("Cannot create host GLES context");return false;
        }
        const char* name=reinterpret_cast<const char*>(glGetString(GL_RENDERER));renderer=name?name:"unknown";
        name=reinterpret_cast<const char*>(glGetString(GL_VENDOR));vendor=name?name:"unknown";
        clientBuffer=reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(eglGetProcAddress("eglGetNativeClientBufferANDROID"));
        createImage=reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        destroyImage=reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        targetImage=reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        createSync=reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
        destroySync=reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
        waitSync=reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(eglGetProcAddress("eglClientWaitSyncKHR"));
        const char* extensions=reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        if(!clientBuffer||!createImage||!destroyImage||!targetImage || !extensions || !std::strstr(extensions,"GL_OES_EGL_image")) {
            reason="Host lacks AHardwareBuffer/EGLImage texture import";return false;
        }
        GLuint vertex=shader(GL_VERTEX_SHADER,"attribute vec2 position; attribute vec2 uv; varying vec2 tex; void main(){gl_Position=vec4(position,0.,1.);tex=uv;}");
        GLuint fragment=shader(GL_FRAGMENT_SHADER,"precision mediump float; varying vec2 tex; uniform sampler2D image; void main(){gl_FragColor=vec4(texture2D(image,tex).rgb,1.);}");
        if(!vertex||!fragment){if(vertex)glDeleteShader(vertex);if(fragment)glDeleteShader(fragment);return false;}
        program=glCreateProgram();glAttachShader(program,vertex);glAttachShader(program,fragment);
        glBindAttribLocation(program,0,"position");glBindAttribLocation(program,1,"uv");glLinkProgram(program);
        glDeleteShader(vertex);glDeleteShader(fragment);GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);
        if(!linked){reason="Cannot link host frame shader";return false;}
        glGenTextures(2,textures);
        for(int i=0;i<2;i++) {
            AHardwareBuffer_Desc description{};description.width=w;description.height=h;description.layers=1;description.format=AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
            description.usage=AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE|AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
            if(AHardwareBuffer_allocate(&description,&buffers[i])!=0){reason="Host cannot allocate shared CPU/GPU RGBA buffer";return false;}
            EGLint attributes[]={EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};
            images[i]=createImage(display,EGL_NO_CONTEXT,EGL_NATIVE_BUFFER_ANDROID,clientBuffer(buffers[i]),attributes);
            if(images[i]==EGL_NO_IMAGE_KHR){failure("Host cannot import shared GPU buffer");return false;}
            glBindTexture(GL_TEXTURE_2D,textures[i]);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            targetImage(GL_TEXTURE_2D,images[i]);
            if(glGetError()!=GL_NO_ERROR){reason="Host rejected EGLImage texture binding";return false;}
        }
        gpu=true;backend="ahardwarebuffer-egl";return true;
    }
    bool cpu(const uint8_t* source,int w,int h) {
        backend="cpu-surface";
        if(ANativeWindow_getWidth(window)!=w || ANativeWindow_getHeight(window)!=h || ANativeWindow_getFormat(window)!=WINDOW_FORMAT_RGBA_8888)
            if(ANativeWindow_setBuffersGeometry(window,w,h,WINDOW_FORMAT_RGBA_8888)!=0)return false;
        ANativeWindow_Buffer buffer{};if(ANativeWindow_lock(window,&buffer,nullptr)!=0)return false;
        bool valid=buffer.bits && buffer.width==w && buffer.height==h && buffer.stride>=w && buffer.format==WINDOW_FORMAT_RGBA_8888;
        if(valid)for(int y=0;y<h;y++)std::memcpy(static_cast<uint8_t*>(buffer.bits)+size_t(y)*buffer.stride*4,source+size_t(y)*w*4,size_t(w)*4);
        int result=ANativeWindow_unlockAndPost(window);return valid && result==0;
    }
    bool present(ANativeWindow* target,const uint8_t* source,int w,int h) {
        if(target!=window){clearWindow();if(window)ANativeWindow_release(window);window=target;}
        else ANativeWindow_release(target);
        if(!attempted && !initialize(w,h))clearGL();
        if(!gpu)return cpu(source,w,h);
        if(output==EGL_NO_SURFACE) {
            EGLint format=0;eglGetConfigAttrib(display,config,EGL_NATIVE_VISUAL_ID,&format);
            if(ANativeWindow_setBuffersGeometry(window,w,h,format)!=0)return false;
            output=eglCreateWindowSurface(display,config,window,nullptr);
        }
        if(output==EGL_NO_SURFACE || !eglMakeCurrent(display,output,output,context)){failure("Host EGL window unavailable");clearWindow();return false;}
        int index=next;
        if(fences[index]!=EGL_NO_SYNC_KHR) {
            EGLint waited=waitSync(display,fences[index],EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,1000000000);
            if(waited!=EGL_CONDITION_SATISFIED_KHR){reason="Host GPU buffer fence timed out";return false;}
            destroySync(display,fences[index]);fences[index]=EGL_NO_SYNC_KHR;
        }
        void* address=nullptr;
        if(AHardwareBuffer_lock(buffers[index],AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,-1,nullptr,&address)!=0){reason="Cannot lock host shared GPU buffer";return false;}
        AHardwareBuffer_Desc description{};AHardwareBuffer_describe(buffers[index],&description);
        for(int y=0;y<h;y++)std::memcpy(static_cast<uint8_t*>(address)+size_t(y)*description.stride*4,source+size_t(y)*w*4,size_t(w)*4);
        int fd=-1;int unlocked=AHardwareBuffer_unlock(buffers[index],&fd);
        if(fd>=0){struct pollfd fence{fd,POLLIN,0};int ready;do{ready=poll(&fence,1,1000);}while(ready<0 && errno==EINTR);close(fd);if(ready<=0){reason="Host CPU-write fence timed out";return false;}}
        if(unlocked!=0){reason="Cannot unlock host shared GPU buffer";return false;}
        EGLint outWidth=0,outHeight=0;eglQuerySurface(display,output,EGL_WIDTH,&outWidth);eglQuerySurface(display,output,EGL_HEIGHT,&outHeight);
        glViewport(0,0,outWidth,outHeight);glDisable(GL_BLEND);glUseProgram(program);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,textures[index]);
        glUniform1i(glGetUniformLocation(program,"image"),0);
        const GLfloat vertices[]={-1,1,0,0, -1,-1,0,1, 1,1,1,0, 1,-1,1,1};
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(GLfloat),vertices);
        glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(GLfloat),vertices+2);
        glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
        if(glGetError()!=GL_NO_ERROR){reason="Host GPU frame draw failed";return false;}
        if(createSync && destroySync && waitSync){fences[index]=createSync(display,EGL_SYNC_FENCE_KHR,nullptr);if(fences[index]==EGL_NO_SYNC_KHR)glFinish();}
        else glFinish();
        if(!eglSwapBuffers(display,output)){failure("Host EGL swap failed");clearWindow();return false;}
        next=1-next;return true;
    }
};
}
extern "C" JNIEXPORT jlong JNICALL Java_dev_kreijstal_aosppocket_runtime_Native_createFramePresenter(JNIEnv*,jclass) {
    return reinterpret_cast<jlong>(new Presenter());
}
extern "C" JNIEXPORT void JNICALL Java_dev_kreijstal_aosppocket_runtime_Native_destroyFramePresenter(JNIEnv*,jclass,jlong handle) {
    delete reinterpret_cast<Presenter*>(handle);
}
extern "C" JNIEXPORT jobjectArray JNICALL Java_dev_kreijstal_aosppocket_runtime_Native_framePresenterInfo(JNIEnv* env,jclass,jlong handle) {
    auto* presenter=reinterpret_cast<Presenter*>(handle);
    jobjectArray result=env->NewObjectArray(4,env->FindClass("java/lang/String"),nullptr);
    if(!result || !presenter)return result;
    const std::string* values[]={&presenter->backend,&presenter->renderer,&presenter->vendor,&presenter->reason};
    for(int i=0;i<4;i++){jstring value=env->NewStringUTF(values[i]->c_str());env->SetObjectArrayElement(result,i,value);env->DeleteLocalRef(value);}return result;
}
extern "C" JNIEXPORT jboolean JNICALL Java_dev_kreijstal_aosppocket_runtime_Native_presentFrame(
    JNIEnv* env,jclass,jlong handle,jobject surface,jobject pixels,jint width,jint height) {
    auto* presenter=reinterpret_cast<Presenter*>(handle);
    auto* source=static_cast<uint8_t*>(env->GetDirectBufferAddress(pixels));jlong size=env->GetDirectBufferCapacity(pixels);
    if(!presenter || !surface || !source || width<1 || height<1 || width>8192 || height>8192 || int64_t(width)*height>16*1024*1024 || size<int64_t(width)*height*4)return JNI_FALSE;
    ANativeWindow* window=ANativeWindow_fromSurface(env,surface);if(!window)return JNI_FALSE;
    bool result=presenter->present(window,source,width,height);
    if(result)presenter->failedFrames=0;
    else if(presenter->gpu && ++presenter->failedFrames>=3) {
        presenter->clearGL();result=presenter->cpu(source,width,height);
    }
    return result?JNI_TRUE:JNI_FALSE;
}
