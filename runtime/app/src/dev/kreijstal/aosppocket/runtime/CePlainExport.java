// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import android.system.*;
import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.security.*;
import java.util.*;
import dev.kreijstal.aosppocket.storage.*;
import dev.kreijstal.aosppocket.storage.CeNamespace.Node;

/**
 * Inverse of {@link CeMigration}: turns encrypted guest CE storage back into
 * the plain layout, where CE is only emulated by the guest and the data rests
 * in this app's own (host CE-encrypted) private storage.
 *
 * The host AppFuse transport that serves encrypted files can wedge the host's
 * single AppFuse bridge thread; on the S20 that held StorageManagerService and
 * vold until the watchdog restarted system_server (2026-11-17/18). Plain
 * storage needs no host AppFuse at all.
 *
 * Requested by the guest root with {@code touch /.pocket-ce-decrypt-requested};
 * runs when the guest stops while its CE storage is unlocked. The export is
 * staged and hash-verified before a journal commits it. The vault is kept as
 * {@code .pocket-ce-v1.exported} until the owner deletes it.
 */
final class CePlainExport {
    static final String REQUEST=".pocket-ce-decrypt-requested";
    private static final String JOURNAL=".pocket-ce-export-verified",VAULT=".pocket-ce-v1",ENABLED=".pocket-ce-enabled",KEPT=".pocket-ce-v1.exported";
    private final File root;
    private final CeNamespace ns;
    private final Map<Integer,String> linked=new HashMap<>();
    private int files,directories,links;
    private long bytes;
    private CePlainExport(File root,CeNamespace ns){this.root=root;this.ns=ns;}
    static boolean requested(File root){return new File(root,REQUEST).isFile();}
    private static File plain(File root,String guest){return new File(root,"system-data/"+guest.substring(6));}

    /**
     * Stages and verifies an export of an unlocked namespace whose guest has
     * stopped, then journals it. Call {@link #finish} once CeStorage is closed:
     * the vault must not move while its store is open. Returns a summary.
     */
    static String perform(File root,CeNamespace ns)throws Exception {
        if(!CeStorage.enabled(root))throw new IOException("Guest storage is not encrypted");
        if(new File(root,JOURNAL).isFile())return "already exported";
        String[] roots=ns.roots();
        if(!Arrays.equals(roots,CeMigration.roots(root)))throw new IOException("CE namespace root layout does not match the guest");
        ns.sync();
        long needed=0;for(int i=0;i<roots.length;i++)needed+=size(ns,ns.node(i+3),new HashSet<>());
        StructStatVfs space=Os.statvfs(root.getPath());
        if(needed+(256L<<20)>space.f_bavail*space.f_frsize)throw new IOException("Not enough free space to decrypt: need "+(needed>>20)+" MiB plus 256 MiB");
        String name=".pocket-ce-plain-"+UUID.randomUUID();Path stage=new File(root,name).toPath();
        boolean committed=false;CePlainExport export=new CePlainExport(root,ns);
        try {
            Files.createDirectory(stage);
            for(int i=0;i<roots.length;i++)export.copy(ns.node(i+3),stage.resolve(Integer.toString(i)).toFile());
            CeMigration.syncDirectory(stage);
            journal(root,name);committed=true;
        } finally {
            if(!committed && Files.exists(stage))remove(stage);
        }
        return export.files+" files, "+export.directories+" directories, "+export.links+" links, "+export.bytes+" bytes";
    }

    /** Completes a verified export; idempotent, safe to call on every start. */
    static void finish(File root)throws Exception {
        File journal=new File(root,JOURNAL);
        if(!journal.isFile())return;
        String name=new String(Files.readAllBytes(journal.toPath()),java.nio.charset.StandardCharsets.US_ASCII).trim();
        if(!name.matches("\\.pocket-ce-plain-[0-9a-f-]{36}"))throw new IOException("Invalid CE export journal");
        Path stage=new File(root,name).toPath();String[] roots=CeMigration.roots(root);
        for(int i=0;i<roots.length;i++) {
            Path staged=stage.resolve(Integer.toString(i));
            File target=plain(root,roots[i]);
            if(!Files.exists(staged))continue; // moved by an interrupted earlier run
            if(Files.isSymbolicLink(target.toPath()))throw new IOException("Unexpected CE root link");
            if(target.exists()) {
                Os.chmod(target.getPath(),0700);
                String[] left=target.list();
                if(left!=null && left.length>0)throw new IOException("Plain CE root "+roots[i]+" is not an empty placeholder; export left staged in "+name);
                Files.delete(target.toPath());
            } else if(!target.getParentFile().isDirectory() && !target.getParentFile().mkdirs())throw new IOException("Cannot create plain CE parent");
            Files.move(staged,target.toPath(),StandardCopyOption.ATOMIC_MOVE);
            CeMigration.syncDirectory(target.getParentFile().toPath());
        }
        Files.deleteIfExists(new File(root,ENABLED).toPath());
        Files.deleteIfExists(new File(root,CeMigration.LAYOUT32).toPath());
        Path vault=new File(root,VAULT).toPath();
        if(Files.exists(vault))Files.move(vault,new File(root,KEPT).toPath(),StandardCopyOption.ATOMIC_MOVE);
        CeMigration.syncDirectory(root.toPath());
        Files.deleteIfExists(new File(root,REQUEST).toPath());
        if(Files.exists(stage))remove(stage);
        Files.delete(journal.toPath());CeMigration.syncDirectory(root.toPath());
    }

    private static long size(CeNamespace ns,Node n,Set<Integer> seen)throws Exception {
        if(!seen.add(n.id))return 0;
        if(n.regular())return ns.store.size(CeNamespace.content(n.id));
        long total=0;if(n.directory())for(int child:n.children.values())total+=size(ns,ns.node(child),seen);
        return total;
    }
    private void copy(Node n,File out)throws Exception {
        if(n.directory()) {
            if(!out.mkdir())throw new IOException("Cannot create "+out);
            directories++;
            for(Map.Entry<String,Integer> child:n.children.entrySet())copy(ns.node(child.getValue()),new File(out,child.getKey()));
            finish(n,out);
            return;
        }
        // Apps may not hard-link their own files (SELinux), so a second name
        // for an inode becomes an independent copy when linking is refused.
        if(n.links>1 && linked.containsKey(n.id))try{Os.link(linked.get(n.id),out.getPath());return;}
        catch(ErrnoException refused){if(refused.errno!=OsConstants.EACCES && refused.errno!=OsConstants.EPERM)throw refused;}
        if(n.symlink()) {
            Os.symlink(hostTarget(n.target),out.getPath());links++;owner(n,out);return;
        }
        if(!n.regular())throw new IOException("Unsupported CE inode type "+Integer.toOctalString(n.mode));
        String content=CeNamespace.content(n.id);long size=ns.store.size(content),offset=0;
        MessageDigest written=MessageDigest.getInstance("SHA-256");byte[] data=new byte[65536];
        try {
            try(FileOutputStream file=new FileOutputStream(out)) {
                while(offset<size) {
                    int count=ns.store.read(content,offset,data,0,(int)Math.min(data.length,size-offset));
                    if(count<=0)throw new IOException("Short encrypted read while exporting");
                    file.write(data,0,count);written.update(data,0,count);offset+=count;
                }
                file.getFD().sync();
            }
            MessageDigest reread=MessageDigest.getInstance("SHA-256");
            try(InputStream in=new FileInputStream(out)){int count;while((count=in.read(data))!=-1)reread.update(data,0,count);}
            if(!MessageDigest.isEqual(written.digest(),reread.digest()))throw new IOException("Exported CE file verification failed");
        } finally {Arrays.fill(data,(byte)0);}
        files++;bytes+=size;
        if(n.links>1)linked.put(n.id,out.getPath());
        finish(n,out);
    }
    // Plain-layout symlinks carry host paths; CeMigration translated them to guest paths.
    private String hostTarget(String target) {
        if(target==null || !target.startsWith("/"))return target;
        if(target.startsWith("/data/"))return root.getPath()+"/system-data/"+target.substring(6);
        return root.getPath()+target;
    }
    private void finish(Node n,File out)throws Exception {
        owner(n,out);
        for(Map.Entry<String,byte[]> attr:n.xattrs.entrySet()) {
            String name=attr.getKey();byte[] value=attr.getValue();
            if(!name.startsWith("user."))continue; // security.selinux is inherited from the host directory
            String child=name.equals("user.inode_cache")?"cache":name.equals("user.inode_code_cache")?"code_cache":null;
            if(child!=null) {
                File dir=new File(out,child);if(!dir.isDirectory())continue;
                value=ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(Os.lstat(dir.getPath()).st_ino).array();
            }
            Os.setxattr(out.getPath(),name,value,0);
        }
        Os.chmod(out.getPath(),n.mode&07777);
        if(n.mtime>0 && !out.setLastModified(n.mtime) && n.regular())throw new IOException("Cannot restore modification time of "+out);
    }
    private void owner(Node n,File out)throws Exception {
        StructStat st=Os.lstat(out.getPath());
        File record=new File(root,".pocket-owner/"+String.format(Locale.ROOT,"%016x-%016x",st.st_dev,st.st_ino));
        // A record left by a deleted pre-migration file may name a reused inode.
        if(n.uid==0 && n.gid==0){Files.deleteIfExists(record.toPath());return;}
        if(!record.getParentFile().isDirectory() && !record.getParentFile().mkdirs())throw new IOException("Cannot create guest owner metadata");
        Files.write(record.toPath(),ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN).putInt(0x504f434b).putInt(n.uid).putInt(n.gid).array());
    }
    private static void journal(File root,String stage)throws IOException {
        Path temp=new File(root,JOURNAL+".new").toPath();
        try(FileOutputStream out=new FileOutputStream(temp.toFile())){out.write((stage+"\n").getBytes(java.nio.charset.StandardCharsets.US_ASCII));out.getFD().sync();}
        Files.move(temp,new File(root,JOURNAL).toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);CeMigration.syncDirectory(root.toPath());
    }
    private static void remove(Path path)throws IOException {
        if(!Files.isSymbolicLink(path) && Files.isDirectory(path)) {
            try{Os.chmod(path.toString(),0700);}catch(ErrnoException ignored){}
            try(DirectoryStream<Path> children=Files.newDirectoryStream(path)){for(Path child:children)remove(child);}
        }
        Files.delete(path);
    }
}
