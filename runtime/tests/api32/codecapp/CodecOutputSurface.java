// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketcodec32test;

import android.graphics.SurfaceTexture;
import android.opengl.*;
import android.os.*;
import android.view.Surface;
import java.nio.*;
import org.json.JSONObject;

/** Real decoder Surface consumer: external texture, EGL draw and pixel readback. */
final class CodecOutputSurface implements AutoCloseable {
    private EGLDisplay display=EGL14.EGL_NO_DISPLAY;
    private EGLContext context=EGL14.EGL_NO_CONTEXT;
    private EGLSurface window=EGL14.EGL_NO_SURFACE;
    private SurfaceTexture texture;
    private Surface surface;
    private HandlerThread callbacks;
    private final Object ready=new Object();
    private int pending,textureId,program;
    private final int width,height;
    private final FloatBuffer vertices;
    private final ByteBuffer pixels;
    private final float[] transform=new float[16];
    String vendor,renderer;
    private static void check(boolean value,String message) {
        if(!value)throw new IllegalStateException(message);
    }
    private static int shader(int type,String source) {
        int id=GLES20.glCreateShader(type);check(id!=0,"No shader object");
        GLES20.glShaderSource(id,source);GLES20.glCompileShader(id);
        int[] status=new int[1];GLES20.glGetShaderiv(id,GLES20.GL_COMPILE_STATUS,status,0);
        if(status[0]==0){String log=GLES20.glGetShaderInfoLog(id);GLES20.glDeleteShader(id);throw new IllegalStateException("Surface shader: "+log);}
        return id;
    }
    CodecOutputSurface(int width,int height) {
        this.width=width;this.height=height;
        vertices=ByteBuffer.allocateDirect(16*4).order(ByteOrder.nativeOrder()).asFloatBuffer();
        vertices.put(new float[]{-1,-1,0,0,1,-1,1,0,-1,1,0,1,1,1,1,1}).position(0);
        pixels=ByteBuffer.allocateDirect(width*height*4).order(ByteOrder.nativeOrder());
        try {
            display=EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);check(display!=EGL14.EGL_NO_DISPLAY,"No consumer EGL display");
            int[] version=new int[2];check(EGL14.eglInitialize(display,version,0,version,1),"Consumer EGL initialization failed");
            EGLConfig[] configs=new EGLConfig[1];int[] count=new int[1];
            check(EGL14.eglChooseConfig(display,new int[]{EGL14.EGL_RED_SIZE,8,EGL14.EGL_GREEN_SIZE,8,EGL14.EGL_BLUE_SIZE,8,
                EGL14.EGL_ALPHA_SIZE,8,EGL14.EGL_RENDERABLE_TYPE,EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_SURFACE_TYPE,EGL14.EGL_PBUFFER_BIT,EGL14.EGL_NONE},0,configs,0,1,count,0) && count[0]>0,"No consumer EGL config");
            context=EGL14.eglCreateContext(display,configs[0],EGL14.EGL_NO_CONTEXT,new int[]{EGL14.EGL_CONTEXT_CLIENT_VERSION,2,EGL14.EGL_NONE},0);
            check(context!=EGL14.EGL_NO_CONTEXT,"Consumer EGL context failed");
            window=EGL14.eglCreatePbufferSurface(display,configs[0],new int[]{EGL14.EGL_WIDTH,width,EGL14.EGL_HEIGHT,height,EGL14.EGL_NONE},0);
            check(window!=EGL14.EGL_NO_SURFACE && EGL14.eglMakeCurrent(display,window,window,context),"Consumer EGL pbuffer failed");
            vendor=GLES20.glGetString(GLES20.GL_VENDOR);renderer=GLES20.glGetString(GLES20.GL_RENDERER);
            check(vendor!=null && renderer!=null,"Missing consumer GL identification");
            int[] ids=new int[1];GLES20.glGenTextures(1,ids,0);textureId=ids[0];check(textureId!=0,"Missing external texture");
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,textureId);
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,GLES20.GL_TEXTURE_MIN_FILTER,GLES20.GL_NEAREST);
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,GLES20.GL_TEXTURE_MAG_FILTER,GLES20.GL_NEAREST);
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,GLES20.GL_TEXTURE_WRAP_S,GLES20.GL_CLAMP_TO_EDGE);
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,GLES20.GL_TEXTURE_WRAP_T,GLES20.GL_CLAMP_TO_EDGE);
            int vertex=shader(GLES20.GL_VERTEX_SHADER,"attribute vec4 position;attribute vec2 uv;uniform mat4 transform;varying vec2 coord;void main(){gl_Position=position;coord=(transform*vec4(uv,0.0,1.0)).xy;}");
            int fragment=0;
            try {
                fragment=shader(GLES20.GL_FRAGMENT_SHADER,"#extension GL_OES_EGL_image_external : require\nprecision mediump float;uniform samplerExternalOES image;varying vec2 coord;void main(){gl_FragColor=texture2D(image,coord);}");
                program=GLES20.glCreateProgram();check(program!=0,"No Surface program");
                GLES20.glAttachShader(program,vertex);GLES20.glAttachShader(program,fragment);GLES20.glLinkProgram(program);
                int[] linked=new int[1];GLES20.glGetProgramiv(program,GLES20.GL_LINK_STATUS,linked,0);
                check(linked[0]!=0,"Surface program link: "+GLES20.glGetProgramInfoLog(program));
            }finally{GLES20.glDeleteShader(vertex);if(fragment!=0)GLES20.glDeleteShader(fragment);}
            texture=new SurfaceTexture(textureId);texture.setDefaultBufferSize(width,height);
            callbacks=new HandlerThread("CodecSurfaceFrames");callbacks.start();
            texture.setOnFrameAvailableListener(value->{synchronized(ready){pending++;ready.notifyAll();}},new Handler(callbacks.getLooper()));
            surface=new Surface(texture);check(GLES20.glGetError()==GLES20.GL_NO_ERROR,"Consumer GL setup failed");
        }catch(RuntimeException error){close();throw error;}
    }
    Surface surface(){return surface;}
    JSONObject read(int frame,long presentationTimeUs)throws Exception {
        long deadline=SystemClock.elapsedRealtime()+5000;
        synchronized(ready) {
            while(pending==0){long left=deadline-SystemClock.elapsedRealtime();check(left>0,"Decoder Surface frame timed out");ready.wait(left);}
            check(pending==1,"Decoder Surface delivered unexpected extra frame");pending=0;
        }
        texture.updateTexImage();texture.getTransformMatrix(transform);
        long timestamp=texture.getTimestamp();check(timestamp==presentationTimeUs*1000,"Decoder Surface timestamp differs");
        GLES20.glViewport(0,0,width,height);GLES20.glUseProgram(program);
        int position=GLES20.glGetAttribLocation(program,"position"),uv=GLES20.glGetAttribLocation(program,"uv");
        int matrix=GLES20.glGetUniformLocation(program,"transform"),image=GLES20.glGetUniformLocation(program,"image");
        check(position>=0 && uv>=0 && matrix>=0 && image>=0,"Surface shader locations missing");
        vertices.position(0);GLES20.glVertexAttribPointer(position,2,GLES20.GL_FLOAT,false,16,vertices);GLES20.glEnableVertexAttribArray(position);
        vertices.position(2);GLES20.glVertexAttribPointer(uv,2,GLES20.GL_FLOAT,false,16,vertices);GLES20.glEnableVertexAttribArray(uv);
        GLES20.glUniformMatrix4fv(matrix,1,false,transform,0);GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,textureId);GLES20.glUniform1i(image,0);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP,0,4);GLES20.glFinish();pixels.clear();
        GLES20.glReadPixels(0,0,width,height,GLES20.GL_RGBA,GLES20.GL_UNSIGNED_BYTE,pixels);
        check(GLES20.glGetError()==GLES20.GL_NO_ERROR,"Decoder Surface pixel readback failed");
        long red=0,green=0,blue=0;int minimum=255,maximum=0,n=0,expected=64+frame*8;
        for(int y=8;y<height-8;y++)for(int x=8;x<width-8;x++) {
            int offset=(y*width+x)*4,r=pixels.get(offset)&255,g=pixels.get(offset+1)&255,b=pixels.get(offset+2)&255,a=pixels.get(offset+3)&255;
            check(Math.abs(r-expected)<=16 && Math.abs(g-expected)<=16 && Math.abs(b-expected)<=16 && a>=250,"Decoder Surface pixel differs");
            red+=r;green+=g;blue+=b;n++;minimum=Math.min(minimum,Math.min(r,Math.min(g,b)));maximum=Math.max(maximum,Math.max(r,Math.max(g,b)));
        }
        return new JSONObject().put("frame",frame).put("presentationTimeUs",presentationTimeUs).put("surfaceTimestampNanos",timestamp)
            .put("expectedGray",expected).put("red",red/(double)n).put("green",green/(double)n).put("blue",blue/(double)n)
            .put("minChannel",minimum).put("maxChannel",maximum).put("sampledPixels",n);
    }
    @Override public void close() {
        if(surface!=null){surface.release();surface=null;}
        if(texture!=null){texture.setOnFrameAvailableListener(null);texture.release();texture=null;}
        if(callbacks!=null){callbacks.quitSafely();try{callbacks.join(1000);}catch(InterruptedException error){Thread.currentThread().interrupt();}callbacks=null;}
        if(display!=EGL14.EGL_NO_DISPLAY) {
            if(context!=EGL14.EGL_NO_CONTEXT && window!=EGL14.EGL_NO_SURFACE) {
                EGL14.eglMakeCurrent(display,window,window,context);
                if(program!=0)GLES20.glDeleteProgram(program);
                if(textureId!=0)GLES20.glDeleteTextures(1,new int[]{textureId},0);
            }
            EGL14.eglMakeCurrent(display,EGL14.EGL_NO_SURFACE,EGL14.EGL_NO_SURFACE,EGL14.EGL_NO_CONTEXT);
            if(window!=EGL14.EGL_NO_SURFACE)EGL14.eglDestroySurface(display,window);
            if(context!=EGL14.EGL_NO_CONTEXT)EGL14.eglDestroyContext(display,context);
            EGL14.eglReleaseThread();EGL14.eglTerminate(display);
        }
        display=EGL14.EGL_NO_DISPLAY;context=EGL14.EGL_NO_CONTEXT;window=EGL14.EGL_NO_SURFACE;program=textureId=0;
    }
}
