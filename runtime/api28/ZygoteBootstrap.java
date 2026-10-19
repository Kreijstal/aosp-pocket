// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

/** Installs the source-built rootless JNI profile before real AOSP ZygoteInit. */
public final class ZygoteBootstrap {
    private static native void install();
    public static void main(String[] args) throws Exception {
        System.load("/system/lib64/libpocket_zygote_api28.so");
        install();
        Class<?> init = Class.forName("com.android.internal.os.ZygoteInit");
        try { init.getMethod("main", String[].class).invoke(null, (Object)args); }
        catch (java.lang.reflect.InvocationTargetException e) {
            Throwable cause = e.getCause();
            if (cause instanceof Error) throw (Error)cause;
            if (cause instanceof Exception) throw (Exception)cause;
            throw e;
        }
    }
}
