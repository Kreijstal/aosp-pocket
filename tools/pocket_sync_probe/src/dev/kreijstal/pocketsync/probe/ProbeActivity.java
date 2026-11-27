// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketsync.probe;

import android.app.Activity;
import android.os.Bundle;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.SecureRandom;
import java.security.Signature;
import java.security.spec.ECGenParameterSpec;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/** End-to-end check for pocket_sync: first launch creates an AES-GCM and an EC
 * key in AndroidKeyStore plus ciphertext in app data; every later launch must
 * decrypt the ciphertext and sign/verify with the copied keys. The verdict is
 * written to files/result.txt and logged with tag PSPROBE. */
public class ProbeActivity extends Activity {
    private static final String AES = "psprobe-aes", EC = "psprobe-ec";

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        String verdict;
        try { verdict = run(); } catch (Throwable t) { verdict = "FAIL " + t; Log.e("PSPROBE", "probe failed", t); }
        Log.i("PSPROBE", verdict);
        try (FileOutputStream o = new FileOutputStream(new File(getFilesDir(), "result.txt"))) {
            o.write((verdict + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (Exception ignored) { }
        finish();
    }

    private String run() throws Exception {
        KeyStore ks = KeyStore.getInstance("AndroidKeyStore");
        ks.load(null);
        File plain = new File(getFilesDir(), "plain.txt"), sealed = new File(getFilesDir(), "sealed.bin");
        if (!ks.containsAlias(AES)) {
            KeyGenerator g = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
            g.init(new KeyGenParameterSpec.Builder(AES, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build());
            SecretKey k = g.generateKey();
            KeyPairGenerator e = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_EC, "AndroidKeyStore");
            e.initialize(new KeyGenParameterSpec.Builder(EC, KeyProperties.PURPOSE_SIGN | KeyProperties.PURPOSE_VERIFY)
                    .setAlgorithmParameterSpec(new ECGenParameterSpec("secp256r1")).setDigests(KeyProperties.DIGEST_SHA256).build());
            e.generateKeyPair();
            byte[] secret = new byte[24];
            new SecureRandom().nextBytes(secret);
            String text = "pocket-sync " + android.util.Base64.encodeToString(secret, android.util.Base64.NO_WRAP);
            Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
            c.init(Cipher.ENCRYPT_MODE, k);
            byte[] ct = c.doFinal(text.getBytes(StandardCharsets.UTF_8)), iv = c.getIV();
            byte[] blob = new byte[1 + iv.length + ct.length];
            blob[0] = (byte) iv.length;
            System.arraycopy(iv, 0, blob, 1, iv.length);
            System.arraycopy(ct, 0, blob, 1 + iv.length, ct.length);
            Files.write(sealed.toPath(), blob);
            Files.write(plain.toPath(), text.getBytes(StandardCharsets.UTF_8));
            return "CREATED " + text;
        }
        byte[] blob = Files.readAllBytes(sealed.toPath());
        int n = blob[0];
        Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
        c.init(Cipher.DECRYPT_MODE, (SecretKey) ks.getKey(AES, null), new GCMParameterSpec(128, blob, 1, n));
        String got = new String(c.doFinal(blob, 1 + n, blob.length - 1 - n), StandardCharsets.UTF_8);
        String want = new String(Files.readAllBytes(plain.toPath()), StandardCharsets.UTF_8);
        if (!got.equals(want)) return "FAIL decrypted text differs";
        Signature s = Signature.getInstance("SHA256withECDSA");
        s.initSign((java.security.PrivateKey) ks.getKey(EC, null));
        s.update(blob);
        byte[] sig = s.sign();
        Signature v = Signature.getInstance("SHA256withECDSA");
        v.initVerify(ks.getCertificate(EC).getPublicKey());
        v.update(blob);
        if (!v.verify(sig)) return "FAIL EC signature does not verify";
        return "OK " + got;
    }
}
