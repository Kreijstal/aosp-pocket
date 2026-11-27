// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import android.system.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.*;
import java.util.*;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/**
 * Emulated file-based encryption, like Android's own emulated FBE: the guest
 * still has credential-encrypted storage that is locked until its credential
 * unlocks it, but the files are plain (resting in this app's host-encrypted
 * private storage) and locking only removes access to the CE roots.
 *
 * No host AppFuse is involved. The guest's disk secret (derived from its
 * synthetic password, stable across credential changes) is checked against
 * an HMAC verifier. While locked the plain CE roots are mode 0400, which the
 * host kernel enforces for every guest process, including guest root: their
 * entries are listed, like the encrypted names of locked FBE, but nothing in
 * them can be reached.
 *
 * Not 0000: the guest framework reads each root's user.serial xattr while
 * locked (UserDataPreparer.reconcileUsers) and destroys the user's data when
 * that fails. Reading a user.* xattr needs read permission on the inode.
 */
final class CeEmulated {
    static final String MARKER=".pocket-ce-emulated";
    private static final String MODES=".pocket-ce-emulated-modes",AUTHLESS=".pocket-ce-emulated-authless",
        PENDING=AUTHLESS+".pending",SECURE=".pocket-ce-emulated-secure.pending",VAULT=".pocket-ce-v1.exported";
    private static final byte[] MAGIC={'P','C','K','E','M','U','0','1'};
    /** Mode of a locked root; 0 is the mode 0.78.182 used. */
    static final int SEALED=0400;
    private static boolean sealed(int mode){return mode==SEALED || mode==0;}
    private CeEmulated(){}
    /** Emulated CE applies only once real CE encryption is off. */
    static boolean enabled(File root){return new File(root,MARKER).isFile() && !CeStorage.enabled(root);}
    private static File plain(File root,String guest){return new File(root,"system-data/"+guest.substring(6));}

    /** Records a verifier for the disk secret of an unlocked store. */
    static void enroll(File root,byte[] secret,boolean authless)throws Exception {
        byte[] salt=new byte[16];new SecureRandom().nextBytes(salt);
        byte[] data=new byte[MAGIC.length+16+32];
        System.arraycopy(MAGIC,0,data,0,8);System.arraycopy(salt,0,data,8,16);System.arraycopy(mac(secret,salt),0,data,24,32);
        if(authless)atomic(root,AUTHLESS,new byte[]{1});else Files.deleteIfExists(new File(root,AUTHLESS).toPath());
        atomic(root,MARKER,data);
    }
    /**
     * Older exports kept the encrypted vault but no verifier. The marker then
     * holds only the magic, and the first unlock checks the secret against
     * the kept vault, then enrolls it.
     */
    static void adoptExport(File root)throws Exception {
        if(new File(root,MARKER).exists() || CeStorage.enabled(root) || !new File(root,VAULT+"/key").isFile())return;
        atomic(root,MARKER,MAGIC.clone());
    }
    private static byte[] mac(byte[] secret,byte[] salt)throws Exception {
        Mac mac=Mac.getInstance("HmacSHA256");mac.init(new SecretKeySpec(secret,"HmacSHA256"));
        mac.update("Pocket emulated CE verifier v1".getBytes(StandardCharsets.US_ASCII));return mac.doFinal(salt);
    }
    /** Throws EACCES unless the secret is the guest's disk secret. */
    static void verify(File root,byte[] secret)throws Exception {
        if(secret.length==0) {
            if(new File(root,AUTHLESS).isFile() || (new File(root,PENDING).isFile() && !new File(root,SECURE).exists()))return;
            throw new dev.kreijstal.aosppocket.storage.CeNamespace.FsException(13);
        }
        byte[] data=Files.readAllBytes(new File(root,MARKER).toPath());
        if(data.length<8 || !Arrays.equals(MAGIC,Arrays.copyOf(data,8)))throw new IOException("Invalid emulated CE marker");
        if(data.length==8) {
            try(dev.kreijstal.aosppocket.storage.EncryptedStore store=dev.kreijstal.aosppocket.storage.EncryptedStore.open(new File(root,VAULT).toPath(),secret)){}
            catch(Exception wrong){throw new dev.kreijstal.aosppocket.storage.CeNamespace.FsException(13);}
            enroll(root,secret,false);return;
        }
        if(data.length!=56)throw new IOException("Invalid emulated CE marker");
        if(!MessageDigest.isEqual(mac(secret,Arrays.copyOfRange(data,8,24)),Arrays.copyOfRange(data,24,56)))throw new dev.kreijstal.aosppocket.storage.CeNamespace.FsException(13);
    }
    /** addUserKeyAuth: an empty secret removes the credential, else it must verify. */
    static void prepareAuth(File root,byte[] secret,boolean unlocked)throws Exception {
        if(secret.length==0) {
            if(!unlocked)throw new dev.kreijstal.aosppocket.storage.CeNamespace.FsException(13);
            atomic(root,PENDING,new byte[]{1});Files.deleteIfExists(new File(root,SECURE).toPath());
        } else {
            verify(root,secret);
            atomic(root,SECURE,new byte[]{1});Files.deleteIfExists(new File(root,PENDING).toPath());
        }
    }
    static void fixate(File root)throws Exception {
        Path secure=new File(root,SECURE).toPath(),pending=new File(root,PENDING).toPath(),active=new File(root,AUTHLESS).toPath();
        if(Files.exists(secure)){Files.deleteIfExists(active);Files.deleteIfExists(pending);Files.delete(secure);}
        else if(Files.exists(pending))Files.move(pending,active,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
        CeMigration.syncDirectory(root.toPath());
    }

    /** Locks: records the CE root modes, then makes each root inaccessible. */
    static synchronized void seal(File root)throws Exception {
        String[] roots=CeMigration.roots(root);
        Properties modes=modes(root);boolean complete=true;
        for(String guest:roots) {
            File dir=plain(root,guest);if(!dir.isDirectory())continue;
            int mode=Os.lstat(dir.getPath()).st_mode&07777;
            if(!sealed(mode))modes.setProperty(guest,Integer.toOctalString(mode));
            else if(modes.getProperty(guest)==null)complete=false;
        }
        if(!complete)throw new IOException("Cannot record the modes of sealed CE roots");
        try(ByteArrayOutputStream out=new ByteArrayOutputStream()){modes.store(out,null);atomic(root,MODES,out.toByteArray());}
        for(String guest:roots){File dir=plain(root,guest);if(dir.isDirectory())Os.chmod(dir.getPath(),SEALED);}
    }
    /** Unlocks: restores the recorded CE root modes. */
    static synchronized void unseal(File root)throws Exception {
        Properties modes=modes(root);
        for(String guest:CeMigration.roots(root)) {
            File dir=plain(root,guest);if(!dir.isDirectory())continue;
            String mode=modes.getProperty(guest);
            if(mode==null){if(sealed(Os.lstat(dir.getPath()).st_mode&07777))throw new IOException("No recorded mode for CE root "+guest);continue;}
            Os.chmod(dir.getPath(),Integer.parseInt(mode,8));
        }
    }
    private static Properties modes(File root)throws IOException {
        Properties modes=new Properties();File file=new File(root,MODES);
        if(file.isFile())try(InputStream in=new FileInputStream(file)){modes.load(in);}
        return modes;
    }
    private static void atomic(File root,String name,byte[] data)throws IOException {
        Path temp=new File(root,name+".new").toPath();
        try(FileOutputStream out=new FileOutputStream(temp.toFile())){out.write(data);out.getFD().sync();}
        try{Os.chmod(temp.toString(),0600);}catch(ErrnoException ignored){}
        Files.move(temp,new File(root,name).toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);CeMigration.syncDirectory(root.toPath());
    }
}
