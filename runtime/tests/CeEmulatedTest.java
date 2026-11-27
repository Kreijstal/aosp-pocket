// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import android.system.*;
import java.io.*;
import java.nio.file.*;
import java.security.*;
import java.util.*;
import dev.kreijstal.aosppocket.storage.*;

/**
 * On-device check of emulated CE: an encrypted guest is exported to plain
 * storage (as an older Manager did, without a verifier), adopted as emulated
 * CE, then locked/unlocked with the right and wrong disk secrets.
 * Run with app_process; argument = scratch dir.
 */
public final class CeEmulatedTest {
    static File root;
    public static void main(String[] args) {
        try{run(args);}catch(Throwable t){t.printStackTrace(System.out);System.out.flush();System.exit(1);}
        System.exit(0);
    }
    static void run(String[] args)throws Exception {
        // Root bypasses the mode-0000 lock; test as an app-like uid.
        if(Os.getuid()==0){Os.setgid(2000);Os.setuid(2000);}
        root=new File(args[0]);
        if(root.exists())CePlainExportRoundTrip.rm(root.toPath());
        new File(root,"system").mkdirs();
        Files.write(new File(root,"system/build.prop").toPath(),"ro.build.version.sdk=32\n".getBytes("UTF-8"));
        CePlainExportRoundTrip.root=root;
        String[] roots=CeMigration.roots(root);
        int[] modes={0771,0770,01771,0770,0771};
        for(int i=0;i<roots.length;i++){File d=CePlainExportRoundTrip.plain(roots[i]);d.mkdirs();Files.write(new File(d,"f"+i).toPath(),("data "+i).getBytes("UTF-8"));Os.chmod(d.getPath(),modes[i%modes.length]);}
        byte[] secret=new byte[32];new SecureRandom().nextBytes(secret);
        byte[] wrong=secret.clone();wrong[0]^=1;
        CeMigration.perform(root,secret.clone());
        new File(root,CePlainExport.REQUEST).createNewFile();
        try(EncryptedStore store=EncryptedStore.open(new File(root,".pocket-ce-v1").toPath(),secret)) {
            CePlainExport.perform(root,new CeNamespace(store,false));
        }
        CePlainExport.finish(root);
        check(!CeEmulated.enabled(root),"emulated before adoption");
        int[] before=new int[roots.length];
        for(int i=0;i<roots.length;i++)before[i]=mode(roots[i]);
        System.out.println("exported root modes "+Arrays.toString(octal(before)));

        // Adopt the verifier-less export, then lock.
        CeEmulated.adoptExport(root);
        check(CeEmulated.enabled(root),"export not adopted");
        check(new File(root,CeEmulated.MARKER).length()==8,"bootstrap marker size");
        CeEmulated.seal(root);
        for(String g:roots)check(mode(g)==CeEmulated.SEALED,"root not sealed "+g);
        check(!new File(CePlainExportRoundTrip.plain(roots[0]),"f0").canRead(),"sealed file readable");
        // The guest framework reads user.serial on locked roots.
        Os.chmod(CePlainExportRoundTrip.plain(roots[1]).getPath(),0700);
        Os.setxattr(CePlainExportRoundTrip.plain(roots[1]).getPath(),"user.serial","0".getBytes("UTF-8"),0);
        Os.chmod(CePlainExportRoundTrip.plain(roots[1]).getPath(),CeEmulated.SEALED);
        check(new String(Os.getxattr(CePlainExportRoundTrip.plain(roots[1]).getPath(),"user.serial"),"UTF-8").equals("0"),"user.serial unreadable while sealed");
        // A root left at 0000 by 0.78.182 still counts as sealed.
        Os.chmod(CePlainExportRoundTrip.plain(roots[2]).getPath(),0);
        CeEmulated.seal(root);check(mode(roots[2])==CeEmulated.SEALED,"0000 root not resealed");
        // Wrong secret against the kept vault.
        check(errno(()->CeEmulated.verify(root,wrong))==13,"wrong secret accepted (vault)");
        check(errno(()->CeEmulated.verify(root,new byte[0]))==13,"empty secret accepted");
        // Right secret: verified by the vault, enrolled.
        CeEmulated.verify(root,secret);
        check(new File(root,CeEmulated.MARKER).length()==56,"not enrolled after vault check");
        check(errno(()->CeEmulated.verify(root,wrong))==13,"wrong secret accepted (verifier)");
        CeEmulated.verify(root,secret);
        CeEmulated.unseal(root);
        for(int i=0;i<roots.length;i++)check(mode(roots[i])==before[i],"mode not restored "+roots[i]+" "+Integer.toOctalString(mode(roots[i])));
        check(new String(Files.readAllBytes(new File(CePlainExportRoundTrip.plain(roots[0]),"f0").toPath()),"UTF-8").equals("data 0"),"data lost");
        // Lock twice, unlock: the mode record must survive a repeated seal.
        CeEmulated.seal(root);CeEmulated.seal(root);CeEmulated.unseal(root);
        for(int i=0;i<roots.length;i++)check(mode(roots[i])==before[i],"mode lost after double seal "+roots[i]);
        // Credential removal: authless only after fixate; re-adding a credential ends it.
        check(errno(()->CeEmulated.prepareAuth(root,new byte[0],false))==13,"authless while locked");
        CeEmulated.prepareAuth(root,new byte[0],true);
        CeEmulated.verify(root,new byte[0]); // pending authless already unlocks, as CeDefaultKey
        CeEmulated.fixate(root);
        CeEmulated.verify(root,new byte[0]);
        CeEmulated.prepareAuth(root,secret,true);
        CeEmulated.fixate(root);
        check(errno(()->CeEmulated.verify(root,new byte[0]))==13,"authless survived a new credential");
        check(errno(()->CeEmulated.prepareAuth(root,wrong,true))==13,"wrong secret enrolled as credential");
        // A fresh enrollment (new exports) verifies directly.
        byte[] other=new byte[32];new SecureRandom().nextBytes(other);
        CeEmulated.enroll(root,other,false);
        CeEmulated.verify(root,other);
        check(errno(()->CeEmulated.verify(root,secret))==13,"old secret accepted after re-enroll");
        System.out.println("CE_EMULATED_PASS");
    }
    interface Action{void run()throws Exception;}
    static int errno(Action a) {
        try{a.run();return 0;}
        catch(CeNamespace.FsException e){return e.errno;}
        catch(Exception e){throw new AssertionError("unexpected "+e,e);}
    }
    static int mode(String guest)throws Exception {return Os.lstat(CePlainExportRoundTrip.plain(guest).getPath()).st_mode&07777;}
    static String[] octal(int[] v){String[] s=new String[v.length];for(int i=0;i<v.length;i++)s[i]=Integer.toOctalString(v[i]);return s;}
    static void check(boolean ok,String what){if(!ok)throw new AssertionError(what);}
}
