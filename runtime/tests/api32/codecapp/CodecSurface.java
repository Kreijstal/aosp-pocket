// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketcodec32test;

import android.opengl.*;
import android.view.Surface;

/** Public EGL producer for MediaCodec's Surface input; no copied YUV input buffers. */
final class CodecSurface implements AutoCloseable {
    private EGLDisplay display = EGL14.EGL_NO_DISPLAY;
    private EGLContext context = EGL14.EGL_NO_CONTEXT;
    private EGLSurface window = EGL14.EGL_NO_SURFACE;
    String vendor, renderer;
    private static void check(boolean ok, String message) {
        if (!ok) throw new IllegalStateException(message + " EGL=0x" + Integer.toHexString(EGL14.eglGetError()));
    }
    CodecSurface(Surface input) {
        try {
            display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
            check(display != EGL14.EGL_NO_DISPLAY, "No EGL display");
            int[] version = new int[2];
            check(EGL14.eglInitialize(display, version, 0, version, 1), "EGL initialization failed");
            int[] attributes = {EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
                0x3142 /* EGL_RECORDABLE_ANDROID */, 1, EGL14.EGL_NONE};
            EGLConfig[] configs = new EGLConfig[1]; int[] count = new int[1];
            check(EGL14.eglChooseConfig(display, attributes, 0, configs, 0, 1, count, 0) && count[0] > 0, "No recordable EGL config");
            context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                new int[]{EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
            check(context != EGL14.EGL_NO_CONTEXT, "EGL context creation failed");
            window = EGL14.eglCreateWindowSurface(display, configs[0], input, new int[]{EGL14.EGL_NONE}, 0);
            check(window != EGL14.EGL_NO_SURFACE, "Encoder EGL window creation failed");
            check(EGL14.eglMakeCurrent(display, window, window, context), "EGL make-current failed");
            vendor = GLES20.glGetString(GLES20.GL_VENDOR); renderer = GLES20.glGetString(GLES20.GL_RENDERER);
            check(vendor != null && renderer != null, "GL identification missing");
        } catch (RuntimeException error) { close(); throw error; }
    }
    void draw(int frame, int width, int height) {
        float gray = (64 + frame * 8) / 255.0f;
        GLES20.glViewport(0, 0, width, height); GLES20.glClearColor(gray, gray, gray, 1);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT); GLES20.glFinish();
        check(GLES20.glGetError() == GLES20.GL_NO_ERROR, "GL drawing failed");
        check(EGLExt.eglPresentationTimeANDROID(display, window, (frame + 1) * 100000000L), "Encoder timestamp failed");
        check(EGL14.eglSwapBuffers(display, window), "Encoder buffer submission failed");
    }
    @Override public void close() {
        if (display != EGL14.EGL_NO_DISPLAY) {
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            if (window != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, window);
            if (context != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, context);
            EGL14.eglReleaseThread(); EGL14.eglTerminate(display);
        }
        display = EGL14.EGL_NO_DISPLAY; context = EGL14.EGL_NO_CONTEXT; window = EGL14.EGL_NO_SURFACE;
    }
}
