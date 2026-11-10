// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyInfo;
import android.security.keystore.KeyProperties;
import android.system.Os;
import java.security.KeyStore;
import java.util.Arrays;
import javax.crypto.*;
import javax.crypto.spec.GCMParameterSpec;

/** Exercises real keystore2 storage and software key operations through JCA. */
public final class KeystoreProbe32 {
    public static void main(String[] args)throws Exception {
        if(android.os.Build.VERSION.SDK_INT!=32 || Os.getuid()!=0)
            throw new AssertionError("Expected isolated 12L root entry");
        Os.setgid(39999);Os.setuid(39999);
        Class.forName("android.security.keystore2.AndroidKeyStoreProvider").getMethod("install").invoke(null);
        KeyStore store=KeyStore.getInstance("AndroidKeyStore");store.load(null);
        String alias="pocket-api32-probe-"+java.util.UUID.randomUUID();
        try {
            KeyGenerator generator=KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES,"AndroidKeyStore");
            generator.init(new KeyGenParameterSpec.Builder(alias,KeyProperties.PURPOSE_ENCRYPT|KeyProperties.PURPOSE_DECRYPT)
                .setKeySize(256).setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).setUserAuthenticationRequired(false).build());
            SecretKey key=generator.generateKey();
            if(key.getEncoded()!=null || !store.containsAlias(alias))throw new AssertionError("Keystore key storage/export failure");
            KeyInfo info=(KeyInfo)SecretKeyFactory.getInstance(key.getAlgorithm(),"AndroidKeyStore").getKeySpec(key,KeyInfo.class);
            if(info.isInsideSecureHardware() || info.getSecurityLevel()!=KeyProperties.SECURITY_LEVEL_SOFTWARE)
                throw new AssertionError("Software key was reported as hardware-backed");
            byte[] plain=java.util.UUID.randomUUID().toString().getBytes("UTF-8");
            Cipher encrypt=Cipher.getInstance("AES/GCM/NoPadding");encrypt.init(Cipher.ENCRYPT_MODE,key);
            byte[] cipher=encrypt.doFinal(plain),iv=encrypt.getIV();
            KeyStore reopened=KeyStore.getInstance("AndroidKeyStore");reopened.load(null);
            SecretKey restored=(SecretKey)reopened.getKey(alias,null);
            Cipher decrypt=Cipher.getInstance("AES/GCM/NoPadding");
            decrypt.init(Cipher.DECRYPT_MODE,restored,new GCMParameterSpec(128,iv));
            if(!Arrays.equals(plain,decrypt.doFinal(cipher)))throw new AssertionError("Stored key AES-GCM round trip failed");
            cipher[cipher.length-1]^=1;
            decrypt.init(Cipher.DECRYPT_MODE,restored,new GCMParameterSpec(128,iv));
            try {decrypt.doFinal(cipher);throw new AssertionError("Tampered ciphertext accepted");}
            catch(AEADBadTagException expected){}
        } finally {store.deleteEntry(alias);}
        if(store.containsAlias(alias))throw new AssertionError("Probe key survived deletion");
        System.out.println("API32_KEYSTORE_CRYPTO_PASS uid=39999 provider=AndroidKeyStore aes_gcm=real security_level=SOFTWARE key_export=denied reopen=1 tamper=rejected cleanup=1");
        System.exit(0);
    }
}
