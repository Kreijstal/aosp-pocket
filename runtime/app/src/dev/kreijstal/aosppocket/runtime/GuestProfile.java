// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.*;
import java.util.Properties;

/** Guest framework ABI, independent of the host's Build.VERSION.SDK_INT. */
final class GuestProfile {
    final int sdk;
    private GuestProfile(int sdk) { this.sdk = sdk; }

    static GuestProfile detect(File root) throws IOException {
        Properties properties = new Properties();
        try (Reader in = new InputStreamReader(new FileInputStream(new File(root, "system/build.prop")), "UTF-8")) {
            properties.load(in);
        }
        String value = properties.getProperty("ro.build.version.sdk", "").trim();
        try { return new GuestProfile(Integer.parseInt(value)); }
        catch (NumberFormatException error) { throw new IOException("Guest has no valid Android SDK version"); }
    }

    void requireBootSupport() throws IOException {
        if (sdk == 25) return;
        if (sdk == 28) throw new IOException("Android 9/API 28 is in Binder bring-up. "
            + "Use the separate API 28 probe; full guest boot is not available yet");
        throw new IOException("Unsupported guest SDK " + sdk + "; boot currently requires the adapted API 25 image");
    }

    void checkDeclaredSdk(int declared) throws IOException {
        if (declared != sdk) throw new IOException("Guest metadata SDK does not match system/build.prop");
    }
}
