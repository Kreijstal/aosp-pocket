// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

/** Installs matching rootless native methods before the real API 32 zygote. */
public final class ZygoteBootstrap32 {
    private static native void install();
    /** Called after ART post-fork hooks and native Binder initialization. */
    private static void startSystemServerBridge()throws Exception {
        ClassLoader loader=new dalvik.system.PathClassLoader("/system/framework/pocket-tools.jar",
            ZygoteBootstrap32.class.getClassLoader());
        Class.forName("dev.kreijstal.aosppocket.guest.InputControl32",true,loader)
            .getMethod("startInSystemServer").invoke(null);
    }
    public static void main(String[] args)throws Exception {
        System.load("/system/lib64/libpocket_zygote_api32.so");
        install();
        if(java.util.Arrays.asList(args).contains("--enable-lazy-preload")) {
            // Lazy zygote startup skips the stock JCA/resource preload. Install
            // the matching provider before framework/app children inherit ART.
            if(java.security.Security.getProvider("AndroidKeyStore")==null)
                Class.forName("android.security.keystore2.AndroidKeyStoreProvider")
                    .getMethod("install").invoke(null);
            if(java.security.Security.getProvider("AndroidKeyStoreBCWorkaround")==null)
                throw new IllegalStateException("12L AndroidKeyStore workaround provider missing");
            java.security.KeyStore.getInstance("AndroidKeyStore");
            java.security.cert.CertificateFactory.getInstance("X.509");
            // Unlike Android 9/10, 12L leaves DEFAULT unset until a system font
            // map is supplied. Use its own preinstalled-font initialization.
            android.graphics.Typeface.class.getMethod("loadPreinstalledSystemFontMap").invoke(null);
            if(android.graphics.Typeface.DEFAULT==null)
                throw new IllegalStateException("12L default typeface unavailable");
            float width=new android.graphics.Paint().measureText("Android 12L");
            if(!(width>0) || !Float.isFinite(width))
                throw new IllegalStateException("12L font measurement failed");
            System.err.println("API32_JCA_FONT_PRELOAD_PASS keystore_provider=1 x509=1 font_width="+width);
        }
        try {
            Class.forName("com.android.internal.os.ZygoteInit").getMethod("main",String[].class)
                .invoke(null,(Object)args);
        }catch(java.lang.reflect.InvocationTargetException error) {
            Throwable cause=error.getCause();
            if(cause instanceof Error)throw (Error)cause;
            if(cause instanceof Exception)throw (Exception)cause;
            throw error;
        }
    }
}
