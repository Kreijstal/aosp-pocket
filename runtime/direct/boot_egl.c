// SPDX-License-Identifier: GPL-3.0-only
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"DIRECT_EGL_FAIL line=%d egl=%x gl=%x\n",__LINE__,eglGetError(),glGetError());return 1; } } while(0)
int main(void) {
    setbuf(stdout,NULL);
    puts("DIRECT_EGL_START");
    EGLDisplay display=eglGetDisplay(EGL_DEFAULT_DISPLAY); CHECK(display!=EGL_NO_DISPLAY);
    EGLint major,minor;CHECK(eglInitialize(display,&major,&minor));
    printf("DIRECT_EGL_INITIALIZED version=%d.%d\n",major,minor);
    EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
                    EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig config;EGLint count;CHECK(eglChooseConfig(display,attrs,&config,1,&count) && count==1);
    EGLint size[]={EGL_WIDTH,4,EGL_HEIGHT,4,EGL_NONE};
    EGLSurface surface=eglCreatePbufferSurface(display,config,size);CHECK(surface!=EGL_NO_SURFACE);
    EGLint version[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,version);CHECK(context!=EGL_NO_CONTEXT);
    puts("DIRECT_EGL_CONTEXT_CREATED");
    CHECK(eglMakeCurrent(display,surface,surface,context));
    printf("DIRECT_EGL_CURRENT renderer=%s version=%s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
    glClearColor(.2f,.4f,.6f,1.f);glClear(GL_COLOR_BUFFER_BIT);
    unsigned char pixel[4]={0};glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    CHECK(glGetError()==GL_NO_ERROR && pixel[0]==51 && pixel[1]==102 && pixel[2]==153 && pixel[3]==255);
    CHECK(eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT));
    CHECK(eglDestroyContext(display,context) && eglDestroySurface(display,surface) && eglTerminate(display));
    puts("DIRECT_EGL_PASS rgba=51,102,153,255");return 0;
}
