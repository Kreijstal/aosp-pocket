// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

/** Installs the matching rootless JNI profile before the real Android 10 zygote. */
public final class ZygoteBootstrap29 {
    private static native void install();
    public static void main(String[] args)throws Exception {
        System.load("/system/lib64/libpocket_zygote_api29.so");
        install();
        if(System.getenv("POCKET_SYSCALL_BACKEND")!=null){
            // Lazy zygote startup skips the normal JCA/font preload. Install
            // the stock Android 10 providers before apps inherit the VM.
            if(java.security.Security.getProvider("AndroidKeyStore")==null)
                Class.forName("android.security.keystore.AndroidKeyStoreProvider").getMethod("install").invoke(null);
            java.security.KeyStore.getInstance("AndroidKeyStore");
            if(java.security.Security.getProvider("AndroidKeyStoreBCWorkaround")==null)
                throw new IllegalStateException("AndroidKeyStore BC workaround missing");
            java.security.cert.CertificateFactory.getInstance("X.509");
            java.security.MessageDigest.getInstance("SHA-256").digest(new byte[]{1,2,3});
            if(android.graphics.Typeface.DEFAULT==null)throw new IllegalStateException("Default typeface missing");
            float width=new android.graphics.Paint().measureText("Android");
            if(!(width>0) || !Float.isFinite(width))throw new IllegalStateException("Font measurement failed");
            System.err.println("API29_PRELOAD_PASS AndroidKeyStore=1 certificate_factory=X.509 digest=SHA-256 font_width="+width);
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
