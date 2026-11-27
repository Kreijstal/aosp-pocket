// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.content.Context;
import android.content.SharedPreferences;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * Owner-selected attestation backend for the direct guests.
 *
 * <p>The value is persisted by the Manager, projected into the guest image
 * before boot and handed to the guest as {@code POCKET_ATTESTATION_BACKEND}.
 * The guest runtime stays authoritative about what a backend can actually do;
 * this class only decides which identity the guest keymaster is asked to
 * present.
 */
final class AttestationBackend {
    static final String LOCAL_SOFTWARE = "local-software";
    static final String HOST_PASSTHROUGH = "host-passthrough";
    static final String TEESIMULATOR = "teesimulator";
    static final List<String> VALUES = Collections.unmodifiableList(
        Arrays.asList(LOCAL_SOFTWARE, HOST_PASSTHROUGH, TEESIMULATOR));
    static final List<String> LABELS = Collections.unmodifiableList(Arrays.asList(
        "Local software (built-in keymaster)",
        "Host passthrough (mirror this device)",
        "TEESimulator (software-attested boot state)"));
    /** Guest-visible projection, relative to the guest image root. */
    static final String MARKER = "system-data/misc/keystore/pocket-attestation-backend";
    /** Android 9 TEESimulator owner opt-in, relative to the guest image root. */
    static final String TEESIM_CONFIG = "system-data/misc/keystore/pocket-teesim.json";
    private static final String PREFERENCES = "guest-attestation";
    private static final String KEY = "backend";

    private AttestationBackend() {}

    static String normalize(String value) {
        if (value != null) for (String candidate : VALUES) if (candidate.equals(value)) return candidate;
        return LOCAL_SOFTWARE;
    }

    static String get(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE);
        return normalize(prefs.getString(KEY, LOCAL_SOFTWARE));
    }

    static void set(Context context, String value) {
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit()
            .putString(KEY, normalize(value)).apply();
    }

    static String label(String value) {
        return LABELS.get(VALUES.indexOf(normalize(value)));
    }

    /** Backend recorded in a prepared image; missing or malformed means the built-in keymaster. */
    static String read(File root) {
        File marker = new File(root, MARKER);
        try {
            return normalize(new String(Files.readAllBytes(marker.toPath()), StandardCharsets.UTF_8).trim());
        } catch (IOException missing) {
            return LOCAL_SOFTWARE;
        }
    }

    /**
     * Project the selected backend into the guest image before boot.
     *
     * <p>{@code local-software} and {@code host-passthrough} never leave an
     * active TEESimulator opt-in behind, because both must be answered by a
     * keymaster that the guest starts itself. {@code teesimulator} keeps the
     * existing owner config intact; it is never created here, since the keybox
     * is owner-supplied material.
     */
    static void apply(File root, String value) throws IOException {
        String backend = normalize(value);
        if (!TEESIMULATOR.equals(backend)) disableTeesimulator(root);
        File marker = new File(root, MARKER);
        File parent = marker.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs())
            throw new IOException("Cannot create the guest keystore directory");
        try {
            if (parent != null) android.system.Os.chmod(parent.getPath(), 0700);
            Files.write(marker.toPath(), (backend + "\n").getBytes(StandardCharsets.UTF_8));
            // The guest keymaster only honours an owner-only projection.
            android.system.Os.chmod(marker.getPath(), 0600);
        } catch (android.system.ErrnoException error) {
            throw new IOException("Cannot project the attestation backend: " + error.getMessage());
        }
    }

    /** Owner recovery: retire an active Android 9 TEESimulator opt-in. Returns true when one existed. */
    static boolean disableTeesimulator(File root) throws IOException {
        File config = new File(root, TEESIM_CONFIG);
        if (!config.isFile()) return false;
        if (!config.getParentFile().getCanonicalPath().startsWith(root.getCanonicalPath() + "/"))
            throw new IOException("Attestation configuration escapes the guest image");
        File saved = new File(config.getParentFile(), "pocket-teesim.disabled-" + System.nanoTime() + ".json");
        Files.move(config.toPath(), saved.toPath(), StandardCopyOption.ATOMIC_MOVE);
        return true;
    }

    /**
     * Drop every guest keystore blob so apps re-create their keys under the
     * currently selected backend. Attestation identity is bound at key
     * creation, so a backend change only takes effect for new keys.
     * Returns the number of removed entries.
     */
    static int resetGuestKeystore(File root) throws IOException {
        File directory = new File(root, "system-data/misc/keystore");
        File canonical = directory.getCanonicalFile();
        if (!canonical.getPath().startsWith(root.getCanonicalPath() + "/"))
            throw new IOException("Guest keystore escapes the guest image");
        File[] entries = canonical.listFiles();
        if (entries == null) return 0;
        int removed = 0;
        for (File entry : entries) {
            String name = entry.getName();
            // Keep the owner's attestation configuration; it is not key material.
            if (name.startsWith("pocket-teesim") || name.equals("pocket-attestation-backend")) continue;
            removed += deleteTree(entry);
        }
        return removed;
    }

    private static int deleteTree(File entry) throws IOException {
        if (entry.isDirectory() && !Files.isSymbolicLink(entry.toPath())) {
            File[] children = entry.listFiles();
            int removed = 1;
            if (children != null) for (File child : children) removed += deleteTree(child);
            Files.delete(entry.toPath());
            return removed;
        }
        Files.deleteIfExists(entry.toPath());
        return 1;
    }
}
