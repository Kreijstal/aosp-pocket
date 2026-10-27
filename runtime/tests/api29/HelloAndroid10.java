// SPDX-License-Identifier: GPL-3.0-only
/** Executed by the guest's real Android 10 ART, rather than the host VM. */
public final class HelloAndroid10 {
    public static void main(String[] args) throws Exception {
        Class<?> blend = Class.forName("android.graphics.BlendMode");
        if (!blend.isEnum() || blend.getEnumConstants().length < 20)
            throw new AssertionError("Android 10 graphics framework unavailable");
        String props = new String(java.nio.file.Files.readAllBytes(java.nio.file.Paths.get("/system/build.prop")), "UTF-8");
        if (!props.contains("ro.build.version.sdk=29\n")) throw new AssertionError("Wrong platform image");
        System.out.println("REAL_ANDROID10_ART_PASS frameworkClass=" + blend.getName()
                + " vm=" + System.getProperty("java.vm.name") + " version=" + System.getProperty("java.vm.version"));
    }
}
