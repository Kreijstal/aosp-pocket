// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.IOException;
import java.nio.file.*;

/** Protect the working image from cross-version Binder library injection. */
public final class GuestProfileTest {
    interface Checked { void run() throws Exception; }
    private static void rejects(Checked action, String expected) throws Exception {
        try { action.run(); throw new AssertionError("Expected rejection: " + expected); }
        catch (IOException error) {
            if (!error.getMessage().contains(expected)) throw error;
        }
    }
    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("pocket-sdk-");
        Path system = Files.createDirectory(root.resolve("system"));
        Path props = system.resolve("build.prop");
        try {
            Files.write(props, "ro.build.version.sdk=25\n".getBytes("UTF-8"));
            GuestProfile.detect(root.toFile()).requireBootSupport();
            rejects(() -> GuestProfile.detect(root.toFile()).checkDeclaredSdk(28), "does not match");
            Files.write(props, "ro.build.version.sdk=28\n".getBytes("UTF-8"));
            GuestProfile.detect(root.toFile()).checkDeclaredSdk(28);
            rejects(() -> GuestProfile.detect(root.toFile()).requireBootSupport(), "Binder bring-up");
            Files.write(props, "ro.build.version.sdk=36\n".getBytes("UTF-8"));
            rejects(() -> GuestProfile.detect(root.toFile()).requireBootSupport(), "Unsupported guest SDK 36");
            Files.write(props, "ro.build.version.sdk=invalid\n".getBytes("UTF-8"));
            rejects(() -> GuestProfile.detect(root.toFile()), "no valid Android SDK");
            System.out.println("PASS: API 25 boot selection; API 28/unknown/malformed/mismatched image guards");
        } finally { Files.deleteIfExists(props); Files.deleteIfExists(system); Files.deleteIfExists(root); }
    }
}
