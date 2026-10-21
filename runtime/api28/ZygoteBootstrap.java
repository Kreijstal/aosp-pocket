// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

/** Installs the source-built rootless JNI profile before real AOSP ZygoteInit. */
public final class ZygoteBootstrap {
    private static native void install();
    public static void main(String[] args) throws Exception {
        System.load("/system/lib64/libpocket_zygote_api28.so");
        install();
        if (System.getenv("POCKET_DIRECT_ROOT")!=null || "proot".equals(System.getenv("POCKET_SYSCALL_BACKEND"))) {
            // --enable-lazy-preload skips ZygoteInit.warmUpJcaProviders().
            // Register the stock providers before forking, so ordinary apps
            // inherit AndroidKeyStore and the matching BC workaround provider.
            // This class is loaded on the guest bootstrap class path; an
            // untrusted app cannot perform this hidden framework call.
            if (java.security.Security.getProvider("AndroidKeyStore")==null) {
                Class.forName("android.security.keystore.AndroidKeyStoreProvider")
                    .getMethod("install").invoke(null);
            }
            java.security.KeyStore.getInstance("AndroidKeyStore");
            if (java.security.Security.getProvider("AndroidKeyStoreBCWorkaround")==null)
                throw new IllegalStateException("AndroidKeyStore BC workaround missing");
            System.err.println("DIRECT_KEYSTORE_PROVIDER_READY AndroidKeyStore=1 AndroidKeyStoreBCWorkaround=1");
            java.security.cert.CertificateFactory.getInstance("X.509");
            java.security.MessageDigest.getInstance("SHA-256").digest(new byte[]{1,2,3});
            System.err.println("DIRECT_JCA_READY certificate_factory=X.509 digest=SHA-256");
            // Lazy zygote startup omits the normal text-resource preload. Set
            // the native default before framework painting can request it.
            Class<?> typeface=Class.forName("android.graphics.Typeface");
            if(typeface.getField("DEFAULT").get(null)==null)throw new IllegalStateException("Default typeface missing");
            Class<?> paintClass=Class.forName("android.graphics.Paint");
            Object paint=paintClass.getConstructor().newInstance();
            float width=(Float)paintClass.getMethod("measureText",String.class).invoke(paint,"Android");
            if(!(width>0) || Float.isInfinite(width))throw new IllegalStateException("Font measurement failed");
            System.err.println("DIRECT_FONTS_READY default_typeface=1 measured_width="+width);
        }
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
