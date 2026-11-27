// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.content.Context;
import android.system.Os;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;

/**
 * Android 12L (API 32) TEESimulator activation.
 *
 * <p>The API 32 guest is a container whose platform files are plain files under
 * the Manager's image root, so the API 9 equivalent of {@code guest_paths.c}'s
 * keystore preload is an {@code LD_PRELOAD} entry for the guest's own init: the
 * guest starts {@code pocket-keystore} (keystore2) and {@code pocket-keymaster}
 * (the stock software Keymaster HAL) from {@code system/etc/init/pocket-runtime.rc}.
 * The attestation identity is produced by the Keymaster HAL, so the preload goes
 * on {@code pocket-keymaster}.
 *
 * <p>Activation is data, not code: an owner-only marker file
 * ({@code system-data/misc/keystore/pocket-attestation-backend}) selects the
 * backend, and an owner-supplied {@code pocket-teesim.json} must exist beside
 * it. Both are projected by {@link AttestationBackend} on the feature branch;
 * this class deliberately duplicates the two paths so it also applies to a tree
 * that does not yet carry that class.
 */
final class AttestationPreload32 {
    /** Same value as {@code AttestationBackend.TEESIMULATOR}. */
    private static final String TEESIMULATOR = "teesimulator";
    /** Same paths as {@code AttestationBackend.MARKER}/{@code TEESIM_CONFIG}. */
    private static final String MARKER = "system-data/misc/keystore/pocket-attestation-backend";
    private static final String CONFIG = "system-data/misc/keystore/pocket-teesim.json";
    private static final String LIBRARY = "system/lib64/libpocket_teesim_api32.so";
    private static final String RC = "system/etc/init/pocket-runtime.rc";
    private static final String RC_BACKUP = "system/etc/init/pocket-runtime.rc.pocket-original";
    private static final String SERVICE = "service pocket-keymaster /system/bin/pocket-crypto-api32";

    private AttestationPreload32() {}

    /** Project (or retire) the preload. Safe to call on every boot. */
    static void apply(Context context, File root) throws IOException {
        File rc = child(root, RC);
        if (!rc.isFile()) return;
        boolean selected = backend(root).equals(TEESIMULATOR);
        File config = child(root, CONFIG);
        File library = child(root, LIBRARY);
        if (selected) {
            // The image is downloaded, so the library and the owner configuration
            // travel in the APK. The library belongs to the Manager and is
            // replaced whenever the APK carries a different build; the owner's
            // pocket-teesim.json is only installed when missing.
            installAsset(context, "teesim/libpocket_teesim_api32.so", library, 0755, true);
            installAsset(context, "teesim/pocket-teesim.json", config, 0644, false);
        }
        boolean wanted = selected && config.isFile() && library.isFile();
        String text = new String(Files.readAllBytes(rc.toPath()), StandardCharsets.UTF_8);
        String entry = ":" + library.getPath().substring(root.getPath().length());
        if (wanted) {
            if (text.contains(entry)) return;
            String patched = insert(text, entry);
            if (patched == null) throw new IOException("pocket-keymaster has no LD_PRELOAD line to extend");
            File backup = child(root, RC_BACKUP);
            if (!backup.isFile())
                Files.copy(rc.toPath(), backup.toPath(), StandardCopyOption.COPY_ATTRIBUTES);
            write(rc, patched);
        } else {
            File backup = child(root, RC_BACKUP);
            if (!backup.isFile()) return;      // nothing was projected
            Files.copy(backup.toPath(), rc.toPath(), StandardCopyOption.REPLACE_EXISTING);
            Files.delete(backup.toPath());
        }
    }

    /**
     * Install an APK asset into the image. With {@code refresh} an existing file
     * is replaced when its bytes differ; without it an existing file wins.
     */
    private static void installAsset(Context context, String asset, File target, int mode,
            boolean refresh) throws IOException {
        if (target.isFile() && !refresh) return;
        File parent = target.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs())
            throw new IOException("Cannot create " + parent);
        Path staged = new File(target.getPath() + ".new").toPath();
        try (InputStream in = context.getAssets().open(asset)) {
            Files.copy(in, staged, StandardCopyOption.REPLACE_EXISTING);
        }
        if (target.isFile() && java.util.Arrays.equals(Files.readAllBytes(staged), Files.readAllBytes(target.toPath()))) {
            Files.delete(staged);
            return;
        }
        chmod(staged.toString(), mode);
        Files.move(staged, target.toPath(), StandardCopyOption.REPLACE_EXISTING);
    }

    /** Backend recorded in the prepared image; a missing marker means built-in Keymaster. */
    static String backend(File root) {
        try {
            return new String(Files.readAllBytes(child(root, MARKER).toPath()), StandardCharsets.UTF_8).trim();
        } catch (IOException | RuntimeException missing) {
            return "local-software";
        }
    }

    /**
     * Append {@code entry} to the {@code setenv LD_PRELOAD} line of the
     * {@code pocket-keymaster} service block. Returns null when the block has no
     * such line (the guest image always does).
     */
    private static String insert(String text, String entry) {
        String[] lines = text.split("\n", -1);
        boolean block = false;
        for (int i = 0; i < lines.length; i++) {
            String line = lines[i];
            if (line.startsWith("service ")) block = line.trim().equals(SERVICE);
            if (!block) continue;
            String trimmed = line.trim();
            if (!trimmed.startsWith("setenv LD_PRELOAD ")) continue;
            lines[i] = line + entry;
            return String.join("\n", lines);
        }
        return null;
    }

    private static void chmod(String path, int mode) throws IOException {
        try {
            Os.chmod(path, mode);
        } catch (android.system.ErrnoException error) {
            throw new IOException("Cannot chmod " + path, error);
        }
    }

    private static void write(File file, String text) throws IOException {
        Path staged = new File(file.getPath() + ".new").toPath();
        Files.write(staged, text.getBytes(StandardCharsets.UTF_8));
        chmod(staged.toString(), 0644);
        Files.move(staged, file.toPath(), StandardCopyOption.REPLACE_EXISTING);
    }

    private static File child(File root, String relative) throws IOException {
        File file = new File(root, relative);
        String canonical = file.getCanonicalPath();
        if (!canonical.startsWith(root.getCanonicalPath() + "/"))
            throw new IOException("Attestation projection escapes the guest image");
        return file;
    }
}
