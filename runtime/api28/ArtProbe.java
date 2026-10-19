// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

/** Runs inside the real Android 9 ART runtime, without Android framework UI. */
public final class ArtProbe {
    public static int answer() { return 28; }
    public static void main(String[] args) throws Exception {
        String vm = System.getProperty("java.vm.name");
        if (!"Dalvik".equals(vm)) throw new AssertionError("Unexpected runtime " + vm);
        final int[] value = {0};
        Thread thread = new Thread(() -> value[0] = answer(), "pocket-art-thread");
        thread.start();
        thread.join();
        if (value[0] != 28 || (Integer)ArtProbe.class.getMethod("answer").invoke(null) != 28)
            throw new AssertionError("Thread/reflection failed");
        boolean caught = false;
        try { Integer.parseInt("expected-exception"); }
        catch (NumberFormatException expected) { caught = true; }
        if (!caught) throw new AssertionError("Exception handling failed");
        Process child = new ProcessBuilder("/system/bin/pocket-property-probe-api28", "check")
            .redirectErrorStream(true).start();
        java.io.ByteArrayOutputStream output = new java.io.ByteArrayOutputStream();
        byte[] buffer = new byte[1024];
        for (int count; (count = child.getInputStream().read(buffer)) != -1;) output.write(buffer, 0, count);
        String text = output.toString("UTF-8");
        if (child.waitFor() != 0 || !text.contains("sdk=28 uid=39999"))
            throw new AssertionError("Java child/property/UID check failed: " + text);
        System.out.println("API28_ART_PASS vm=" + vm + " version=" + System.getProperty("java.vm.version")
            + " threads=passed reflection=passed exceptions=passed child_properties=28 child_uid=39999");
    }
}
