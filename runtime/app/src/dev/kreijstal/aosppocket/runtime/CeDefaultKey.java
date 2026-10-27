// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import android.security.keystore.*;
import android.system.Os;
import java.io.*;
import java.nio.file.*;
import java.security.*;
import java.util.*;
import javax.crypto.*;
import javax.crypto.spec.GCMParameterSpec;

/** Authless CE key slot, used only when the guest explicitly removes its lock. */
final class CeDefaultKey {
    private static final String ALIAS="Pocket.CE.default.v1";
    private static final String ACTIVE=".pocket-ce-authless",PENDING=ACTIVE+".pending",SECURE=".pocket-ce-secure.pending";
    private static final byte[] MAGIC={'P','C','K','D','E','0','0','1'};
    private static SecretKey key(boolean create)throws Exception {
        KeyStore store=KeyStore.getInstance("AndroidKeyStore");store.load(null);
        if(store.containsAlias(ALIAS))return (SecretKey)store.getKey(ALIAS,null);
        if(!create)throw new IOException("Host default-unlock key is unavailable");
        KeyGenerator gen=KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES,"AndroidKeyStore");
        gen.init(new KeyGenParameterSpec.Builder(ALIAS,KeyProperties.PURPOSE_ENCRYPT|KeyProperties.PURPOSE_DECRYPT)
            .setKeySize(256).setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setRandomizedEncryptionRequired(true).build());return gen.generateKey();
    }
    static void ensure()throws Exception {key(true);}
    private static byte[] aad(File root)throws Exception {
        MessageDigest digest=MessageDigest.getInstance("SHA-256");digest.update("Pocket CE authless slot v1".getBytes("UTF-8"));
        Path envelope=new File(root,".pocket-ce-v1/key").toPath();if(Files.size(envelope)!=116)throw new IOException("Invalid CE key envelope");
        return digest.digest(Files.readAllBytes(envelope));
    }
    private static void atomic(File root,String name,byte[] data)throws Exception {
        Path temp=new File(root,name+".new").toPath();
        try(FileOutputStream out=new FileOutputStream(temp.toFile())){Os.chmod(temp.toString(),0600);out.write(data);out.getFD().sync();}
        Files.move(temp,new File(root,name).toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);CeMigration.syncDirectory(root.toPath());
    }
    static void prepare(File root,byte[] secret)throws Exception {
        Cipher cipher=Cipher.getInstance("AES/GCM/NoPadding");cipher.init(Cipher.ENCRYPT_MODE,key(true));cipher.updateAAD(aad(root));
        byte[] iv=cipher.getIV();if(iv.length!=12)throw new IOException("Unsupported host GCM nonce size");byte[] sealed=cipher.doFinal(secret);
        byte[] data=new byte[20+sealed.length];System.arraycopy(MAGIC,0,data,0,8);System.arraycopy(iv,0,data,8,12);System.arraycopy(sealed,0,data,20,sealed.length);
        atomic(root,PENDING,data);Files.deleteIfExists(new File(root,SECURE).toPath());CeMigration.syncDirectory(root.toPath());
    }
    static void prepareSecure(File root)throws Exception {
        atomic(root,SECURE,new byte[]{1});Files.deleteIfExists(new File(root,PENDING).toPath());CeMigration.syncDirectory(root.toPath());
    }
    static byte[] load(File root)throws Exception {
        File file=new File(root,ACTIVE);
        if(!file.isFile() && !new File(root,SECURE).exists())file=new File(root,PENDING);
        if(!file.isFile())throw new SecurityException("Guest credential is required");
        if(file.length()<68 || file.length()>4096)throw new IOException("Invalid default-unlock slot size");
        byte[] data=Files.readAllBytes(file.toPath());
        if(data.length<68 || data.length>4096 || !Arrays.equals(MAGIC,Arrays.copyOf(data,8)))throw new IOException("Invalid default-unlock slot");
        Cipher cipher=Cipher.getInstance("AES/GCM/NoPadding");cipher.init(Cipher.DECRYPT_MODE,key(false),new GCMParameterSpec(128,data,8,12));cipher.updateAAD(aad(root));
        byte[] secret=cipher.doFinal(data,20,data.length-20);if(secret.length<32){Arrays.fill(secret,(byte)0);throw new IOException("Invalid default-unlock secret");}return secret;
    }
    static void fixate(File root)throws Exception {
        Path secure=new File(root,SECURE).toPath(),pending=new File(root,PENDING).toPath(),active=new File(root,ACTIVE).toPath();
        if(Files.exists(secure)) {
            Files.deleteIfExists(active);Files.deleteIfExists(pending);CeMigration.syncDirectory(root.toPath());Files.delete(secure);
        } else if(Files.exists(pending))Files.move(pending,active,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
        CeMigration.syncDirectory(root.toPath());
    }
}
