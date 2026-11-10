// SPDX-License-Identifier: GPL-3.0-only
/** Executed by the guest's real API 32 ART. No host Java or account is involved. */
public final class HelloAndroid12L {
    public static void main(String[] args)throws Exception {
        // Spatializer was added in API 32, unlike API 31's initial Android 12.
        // Inspect its API without instantiating a service or claiming audio support.
        Class<?> spatializer=Class.forName("android.media.Spatializer",false,null);
        if(spatializer.getMethod("getImmersiveAudioLevel").getReturnType()!=int.class)
            throw new AssertionError("Android 12L framework API is unavailable");
        String props=new String(java.nio.file.Files.readAllBytes(java.nio.file.Paths.get("/system/build.prop")),"UTF-8");
        if(!props.contains("ro.build.version.sdk=32\n"))throw new AssertionError("Wrong guest platform");
        System.out.println("REAL_ANDROID12L_ART_PASS frameworkClass="+spatializer.getName()
                +" vm="+System.getProperty("java.vm.name")+" version="+System.getProperty("java.vm.version"));
    }
}
