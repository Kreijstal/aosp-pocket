// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.File;
import java.nio.file.Files;

/** Host test: a guest keystore reset moves key files aside (undoable), keeps
 * the attestation configuration, and keeps at most KEPT_RESETS backups. */
public final class KeystoreResetTest {
    public static void main(String[] args) throws Exception {
        File root = Files.createTempDirectory("keystore-reset").toFile();
        File keystore = new File(root, "system-data/misc/keystore"), misc = keystore.getParentFile();
        for (int round = 0; round < 5; round++) {
            keystore.mkdirs();
            Files.write(new File(keystore, "persistent.sqlite").toPath(), ("db" + round).getBytes());
            Files.write(new File(keystore, "pocket-attestation-backend").toPath(), "local-software\n".getBytes());
            File backup = AttestationBackend.resetGuestKeystore(root);
            check(backup != null && new File(backup, "persistent.sqlite").isFile(), "key database moved into the backup");
            check(new String(Files.readAllBytes(new File(backup, "persistent.sqlite").toPath())).equals("db" + round), "backup holds this round's data");
            check(!new File(keystore, "persistent.sqlite").exists(), "key database removed from the live keystore");
            check(new File(keystore, "pocket-attestation-backend").isFile(), "attestation setting kept");
            Thread.sleep(1100); // backups are named per second
        }
        String[] kept = misc.list((d, n) -> n.startsWith("keystore-reset-"));
        check(kept.length == AttestationBackend.KEPT_RESETS, "only the newest " + AttestationBackend.KEPT_RESETS + " backups kept, found " + kept.length);
        check(AttestationBackend.resetGuestKeystore(root) == null, "empty keystore reports nothing to reset");
        System.out.println("PASS: keystore reset is undoable, keeps the attestation setting, prunes to " + AttestationBackend.KEPT_RESETS + " backups");
    }
    private static void check(boolean ok, String what) { if (!ok) throw new AssertionError(what); }
}
