// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import android.system.*;
import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.security.*;
import java.util.*;
import dev.kreijstal.aosppocket.storage.*;

/**
 * On-device round trip: plain tree -> CeMigration (encrypt) -> CePlainExport
 * (decrypt) -> identical plain tree. Run with app_process; argument = scratch dir.
 */
public final class CePlainExportRoundTrip {
    static File root;
    public static void main(String[] args) {
        try{run(args);}catch(Throwable t){t.printStackTrace(System.out);System.out.flush();System.exit(1);}
        System.exit(0);
    }
    static void run(String[] args)throws Exception {
        root=new File(args[0]);
        if(root.exists())rm(root.toPath());
        new File(root,"system").mkdirs();
        Files.write(new File(root,"system/build.prop").toPath(),"ro.build.version.sdk=32\n".getBytes("UTF-8"));
        String[] roots=CeMigration.roots(root);
        for(String guest:roots)plain(guest).mkdirs();
        File app=new File(plain(roots[0]),"com.example.app");
        File dbs=new File(app,"databases"),cache=new File(app,"cache"),code=new File(app,"code_cache"),prefs=new File(app,"shared_prefs");
        dbs.mkdirs();cache.mkdirs();code.mkdirs();prefs.mkdirs();
        byte[] big=new byte[3*1024*1024+17];new Random(7).nextBytes(big);
        Files.write(new File(dbs,"main.db").toPath(),big);
        Files.write(new File(dbs,"main.db-journal").toPath(),new byte[0]);
        Files.write(new File(prefs,"p.xml").toPath(),"<map><int name=\"a\" value=\"1\"/></map>".getBytes("UTF-8"));
        Os.symlink(root.getPath()+"/system-data/"+roots[0].substring(6)+"/com.example.app/databases/main.db",new File(app,"abs-link").getPath());
        Os.symlink("databases/main.db",new File(app,"rel-link").getPath());
        Os.chmod(new File(dbs,"main.db").getPath(),0660);Os.chmod(prefs.getPath(),0771);Os.chmod(new File(prefs,"p.xml").getPath(),0600);
        Os.setxattr(app.getPath(),"user.inode_cache",le(Os.stat(cache.getPath()).st_ino),0);
        Os.setxattr(app.getPath(),"user.inode_code_cache",le(Os.stat(code.getPath()).st_ino),0);
        Os.setxattr(new File(dbs,"main.db").getPath(),"user.pocket.test","hello".getBytes("UTF-8"),0);
        owner(app,10123,10123);owner(dbs,10123,10123);owner(new File(dbs,"main.db"),10123,1023);owner(new File(prefs,"p.xml"),10123,10123);
        for(int i=1;i<roots.length;i++)Files.write(new File(plain(roots[i]),"marker-"+i).toPath(),("root "+i).getBytes("UTF-8"));
        new File(app,"databases").setLastModified(1700000000000L);new File(dbs,"main.db").setLastModified(1700000123000L);

        Map<String,String> before=snapshot(roots);
        byte[] secret=new byte[32];new SecureRandom().nextBytes(secret);
        CeMigration.perform(root,secret.clone());
        check(CeStorage.enabled(root),"migration did not enable CE");
        String[] left=plain(roots[0]).list();check(left==null || left.length==0,"plaintext left after migration");

        new File(root,CePlainExport.REQUEST).createNewFile();
        String summary;
        try(EncryptedStore store=EncryptedStore.open(new File(root,".pocket-ce-v1").toPath(),secret)) {
            summary=CePlainExport.perform(root,new CeNamespace(store,false));
        }
        CePlainExport.finish(root);
        check(!CeStorage.enabled(root),"CE still enabled");
        check(new File(root,".pocket-ce-v1.exported").isDirectory(),"vault backup missing");
        check(!new File(root,CePlainExport.REQUEST).exists(),"request not cleared");
        Map<String,String> after=snapshot(roots);
        int bad=0;
        for(String k:new TreeSet<>(union(before.keySet(),after.keySet()))) {
            String a=before.get(k),b=after.get(k);
            if(!Objects.equals(a,b)){bad++;System.out.println("MISMATCH "+k+"\n  before "+a+"\n  after  "+b);}
        }
        System.out.println("export: "+summary+"; entries "+before.size()+"; mismatches "+bad);
        if(bad!=0)throw new AssertionError("round trip differs");
        System.out.println("CE_PLAIN_ROUNDTRIP_PASS");
    }
    static Set<String> union(Set<String> a,Set<String> b){Set<String> s=new HashSet<>(a);s.addAll(b);return s;}
    static File plain(String guest){return new File(root,"system-data/"+guest.substring(6));}
    static byte[] le(long v){return ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(v).array();}
    static void owner(File f,int uid,int gid)throws Exception {
        StructStat st=Os.lstat(f.getPath());File r=new File(root,".pocket-owner/"+String.format(Locale.ROOT,"%016x-%016x",st.st_dev,st.st_ino));
        r.getParentFile().mkdirs();Files.write(r.toPath(),ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN).putInt(0x504f434b).putInt(uid).putInt(gid).array());
    }
    static String ownerOf(StructStat st)throws Exception {
        File r=new File(root,".pocket-owner/"+String.format(Locale.ROOT,"%016x-%016x",st.st_dev,st.st_ino));
        if(!r.isFile())return "0:0";ByteBuffer b=ByteBuffer.wrap(Files.readAllBytes(r.toPath())).order(ByteOrder.LITTLE_ENDIAN);b.getInt();return b.getInt()+":"+b.getInt();
    }
    static Map<String,String> snapshot(String[] roots)throws Exception {
        Map<String,String> out=new TreeMap<>();Map<Long,String> inodes=new HashMap<>();
        for(String guest:roots)walk(plain(guest),guest,out,inodes);
        return out;
    }
    static void walk(File f,String guest,Map<String,String> out,Map<Long,String> inodes)throws Exception {
        StructStat st=Os.lstat(f.getPath());int type=st.st_mode&0170000;
        StringBuilder d=new StringBuilder(Integer.toOctalString(st.st_mode)).append(" own=").append(ownerOf(st));
        if(type==CeNamespace.LNK)d.append(" ->").append(Os.readlink(f.getPath()));
        else {
            for(String x:new TreeSet<>(Arrays.asList(Os.listxattr(f.getPath()))))if(x.startsWith("user.")) {
                byte[] v=Os.getxattr(f.getPath(),x);
                if(x.equals("user.inode_cache"))v=le(ByteBuffer.wrap(v).order(ByteOrder.LITTLE_ENDIAN).getLong()==Os.stat(new File(f,"cache").getPath()).st_ino?1:-1);
                if(x.equals("user.inode_code_cache"))v=le(ByteBuffer.wrap(v).order(ByteOrder.LITTLE_ENDIAN).getLong()==Os.stat(new File(f,"code_cache").getPath()).st_ino?1:-1);
                d.append(' ').append(x).append('=').append(Base64.getEncoder().encodeToString(v));
            }
        }
        if(type==CeNamespace.REG) {
            MessageDigest md=MessageDigest.getInstance("SHA-256");md.update(Files.readAllBytes(f.toPath()));
            d.append(" size=").append(st.st_size).append(" sha=").append(Base64.getEncoder().encodeToString(md.digest())).append(" mtime=").append(st.st_mtime);
            if(st.st_nlink>1){String first=inodes.putIfAbsent(st.st_ino,guest);d.append(" link=").append(first==null?guest:first);}
        }
        out.put(guest,d.toString());
        if(type==CeNamespace.DIR) {String[] names=f.list();Arrays.sort(names);for(String n:names)walk(new File(f,n),guest+"/"+n,out,inodes);}
    }
    static void check(boolean ok,String what){if(!ok)throw new AssertionError(what);}
    static void rm(Path p)throws IOException {
        if(!Files.isSymbolicLink(p) && Files.isDirectory(p)){try{Os.chmod(p.toString(),0700);}catch(ErrnoException e){}try(DirectoryStream<Path> c=Files.newDirectoryStream(p)){for(Path x:c)rm(x);}}
        Files.delete(p);
    }
}
