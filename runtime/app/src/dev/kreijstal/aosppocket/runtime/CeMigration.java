// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import android.system.*;
import java.io.*;
import java.nio.*;
import java.nio.channels.FileChannel;
import java.nio.file.*;
import java.security.*;
import java.util.*;
import dev.kreijstal.aosppocket.storage.*;
import dev.kreijstal.aosppocket.storage.CeNamespace.Node;

/** Offline, verified migration. Source removal starts only after durable proof. */
final class CeMigration {
    private static final String VERIFIED=".pocket-ce-migration-verified",VAULT=".pocket-ce-v1",ENABLED=".pocket-ce-enabled";
    static final String LAYOUT32=".pocket-ce-layout-12l";
    private final File root;
    private final CeNamespace namespace;
    private final Map<String,String> hardlinks=new HashMap<>();
    private final Map<Integer,byte[]> hashes=new HashMap<>();
    private int files;
    private CeMigration(File root,CeNamespace namespace){this.root=root;this.namespace=namespace;namespace.autoCommit=false;}
    private static File source(File root,String guest){return new File(root,"system-data/"+guest.substring(6));}
    static boolean api32(File root)throws IOException {
        File properties=new File(root,"system/build.prop");
        if(!properties.isFile())return false;
        String text="\n"+new String(Files.readAllBytes(properties.toPath()),java.nio.charset.StandardCharsets.UTF_8)+"\n";
        return text.contains("\nro.build.version.sdk=32\n");
    }
    static String[] roots(File root)throws IOException {return (api32(root)?CeNamespace.API32_ROOTS:CeNamespace.ROOTS).clone();}
    static void syncDirectory(Path path)throws IOException {try(FileChannel fd=FileChannel.open(path,StandardOpenOption.READ)){fd.force(true);}}
    private static void marker(File root,String name)throws IOException {
        marker(root,name,"1\n");
    }
    private static void marker(File root,String name,String value)throws IOException {
        Path temp=new File(root,name+".new").toPath();
        try(FileOutputStream out=new FileOutputStream(temp.toFile())){out.write(value.getBytes(java.nio.charset.StandardCharsets.US_ASCII));out.getFD().sync();}
        Files.move(temp,new File(root,name).toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);syncDirectory(root.toPath());
    }
    static void finishVerified(File root)throws Exception {
        if(!new File(root,VERIFIED).isFile())return;
        String[] journal=new String(Files.readAllBytes(new File(root,VERIFIED).toPath()),java.nio.charset.StandardCharsets.US_ASCII).trim().split("\n",-1);
        boolean layout32=journal.length==2 && journal[1].equals("api32-roots=5");
        if((journal.length!=1 && !layout32) || api32(root)!=layout32)throw new IOException("CE migration layout does not match the guest; plaintext preserved");
        String staging=journal[0];
        if(!staging.matches("\\.pocket-ce-stage-[0-9a-f-]{36}"))throw new IOException("Invalid CE migration journal");
        Path stage=new File(root,staging).toPath(),vault=new File(root,VAULT).toPath();
        if(!Files.exists(vault)) {
            if(!Files.isRegularFile(stage.resolve("state")) || !Files.isRegularFile(stage.resolve("key")))throw new IOException("Verified CE staging data is missing");
            Files.move(stage,vault,StandardCopyOption.ATOMIC_MOVE);syncDirectory(root.toPath());
        }
        if(!new File(root,VAULT+"/state").isFile() || !new File(root,VAULT+"/key").isFile())throw new IOException("Verified CE vault is missing; refusing migration cleanup");
        if(layout32)marker(root,LAYOUT32);
        for(String guest:roots(root)) {
            File plain=source(root,guest);
            if(Files.isSymbolicLink(plain.toPath()))throw new IOException("Unexpected CE source root link");
            if(plain.exists())Os.chmod(plain.getPath(),0700);
            if(plain.exists())try(DirectoryStream<Path> children=Files.newDirectoryStream(plain.toPath())) {for(Path child:children)remove(child);}
            else if(!plain.mkdirs())throw new IOException("Cannot create locked CE root placeholder");
            for(String attr:Os.listxattr(plain.getPath()))if(attr.startsWith("user."))Os.removexattr(plain.getPath(),attr);
            seal(plain);
        }
        marker(root,ENABLED);Files.delete(new File(root,VERIFIED).toPath());syncDirectory(root.toPath());
    }
    private static void seal(File directory)throws Exception {
        try(FileChannel fd=FileChannel.open(directory.toPath(),StandardOpenOption.READ)){Os.chmod(directory.getPath(),0000);fd.force(true);}
    }
    static void sealPlaceholders(File root)throws Exception {
        if(!CeStorage.enabled(root))return;
        if(api32(root) && !new File(root,LAYOUT32).isFile())throw new IOException("Android12L CE storage requires the five-root layout");
        for(String guest:roots(root)) {
            File plain=source(root,guest);if(Files.isSymbolicLink(plain.toPath()))throw new IOException("Unexpected plaintext CE root link");
            Os.chmod(plain.getPath(),0700);
            try(DirectoryStream<Path> paths=Files.newDirectoryStream(plain.toPath())) {if(paths.iterator().hasNext())throw new IOException("Unexpected plaintext CE data; refusing to bypass encrypted storage");}
            seal(plain);
        }
    }
    static void perform(File root,byte[] secret)throws Exception {
        finishVerified(root);if(CeStorage.enabled(root))throw new IOException("CE migration is already complete");
        Path finalVault=new File(root,VAULT).toPath();if(Files.exists(finalVault))throw new IOException("Uncommitted CE vault needs owner recovery; plaintext has been preserved");
        boolean layout32=api32(root);String[] roots=roots(root);
        Path stage=new File(root,".pocket-ce-stage-"+UUID.randomUUID()).toPath();boolean verified=false;
        try {
            Map<Integer,byte[]> expected;
            try(EncryptedStore store=EncryptedStore.create(stage,secret)) {
                CeNamespace namespace=new CeNamespace(store,true,layout32);CeMigration migration=new CeMigration(root,namespace);
                for(int i=0;i<roots.length;i++) {
                    String guest=roots[i];File plain=source(root,guest);
                    if(Files.isSymbolicLink(plain.toPath()))throw new IOException("CE source root must be a real directory");
                    if(plain.exists()) {
                        migration.attributes(namespace.node(i+3),Os.lstat(plain.getPath()),plain);
                        migration.children(plain,guest);
                        migration.attributes(namespace.node(i+3),Os.lstat(plain.getPath()),plain);
                    }
                }
                namespace.sync();expected=migration.hashes;
            }
            try(EncryptedStore store=EncryptedStore.open(stage,secret)) {
                CeNamespace ns=new CeNamespace(store,false);byte[] data=new byte[65536];
                if(!Arrays.equals(ns.roots(),roots))throw new IOException("Migrated CE namespace has the wrong root layout");
                try {
                    for(Map.Entry<Integer,byte[]> item:expected.entrySet()) {
                        MessageDigest digest=MessageDigest.getInstance("SHA-256");String name=CeNamespace.content(item.getKey());long size=store.size(name),offset=0;
                        while(offset<size){int n=store.read(name,offset,data,0,(int)Math.min(data.length,size-offset));if(n<=0)throw new IOException("Short encrypted migration read");digest.update(data,0,n);offset+=n;}
                        if(!MessageDigest.isEqual(item.getValue(),digest.digest()))throw new IOException("CE migration content verification failed");
                        ns.node(item.getKey());
                    }
                }finally{Arrays.fill(data,(byte)0);}
            }
            marker(root,VERIFIED,stage.getFileName().toString()+"\n"+(layout32?"api32-roots=5\n":""));verified=true;
            // After this journal commit, startup can finish removal without
            // retaining the password or key. Before it, plaintext is intact.
            finishVerified(root);
        } finally {
            if(!verified && Files.exists(stage))remove(stage);
        }
    }
    private void children(File directory,String guest)throws Exception {
        try(DirectoryStream<Path> paths=Files.newDirectoryStream(directory.toPath())) {
            for(Path path:paths)copy(path.toFile(),guest+"/"+path.getFileName());
        }
    }
    private void copy(File file,String guest)throws Exception {
        StructStat st=Os.lstat(file.getPath());int type=st.st_mode&0170000;
        if(type!=CeNamespace.REG && type!=CeNamespace.DIR && type!=CeNamespace.LNK)throw new IOException("Unsupported special file in CE migration");
        String inode=st.st_dev+":"+st.st_ino;
        if(type!=CeNamespace.DIR && hardlinks.containsKey(inode)) {namespace.link(hardlinks.get(inode),guest,0,0,new int[0]);return;}
        String target=type==CeNamespace.LNK?Os.readlink(file.getPath()):null;
        if(target!=null && target.startsWith(root.getPath()+"/system-data/"))target="/data/"+target.substring(root.getPath().length()+13);
        else if(target!=null && target.startsWith(root.getPath()+"/"))target=target.substring(root.getPath().length());
        Node node=namespace.create(guest,st.st_mode,0,0,new int[0],target);hardlinks.put(inode,guest);attributes(node,st,file);
        if(type==CeNamespace.DIR){children(file,guest);attributes(node,st,file);}
        if(type==CeNamespace.REG) {
            MessageDigest digest=MessageDigest.getInstance("SHA-256");byte[] data=new byte[65536];long offset=0;
            try(InputStream input=new FileInputStream(file)) {
                int n;while((n=input.read(data))!=-1){namespace.store.write(CeNamespace.content(node.id),offset,data,0,n);digest.update(data,0,n);offset+=n;}
            }finally{Arrays.fill(data,(byte)0);}
            if(offset!=st.st_size)throw new IOException("CE source changed during offline migration");hashes.put(node.id,digest.digest());
        }
        if(++files%32==0)namespace.sync();
    }
    private void attributes(Node node,StructStat st,File file)throws Exception {
        int uid=0,gid=0;
        String name=String.format(Locale.ROOT,"%016x-%016x",st.st_dev,st.st_ino);File owner=new File(root,".pocket-owner/"+name);
        if(owner.isFile()) {
            byte[] raw=Files.readAllBytes(owner.toPath());if(raw.length!=12)throw new IOException("Invalid guest owner metadata");
            ByteBuffer b=ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN);if(b.getInt()!=0x504f434b)throw new IOException("Invalid guest owner marker");uid=b.getInt();gid=b.getInt();
        }
        node.mode=st.st_mode;node.uid=uid;node.gid=gid;node.atime=st.st_atime*1000;node.mtime=st.st_mtime*1000;node.ctime=st.st_ctime*1000;
        node.xattrs.clear();
        // Preserve the inherited host label as read-only filesystem metadata.
        // installd reads it even when guest restorecon is disabled. This does
        // not install a guest policy or relabel any host inode.
        if(!node.symlink())for(String attr:Os.listxattr(file.getPath()))if(attr.startsWith("user.") || attr.equals("security.selinux")) {
            byte[] value=Os.getxattr(file.getPath(),attr);if(value.length>16384)throw new IOException("CE migration attribute bound");
            String child=attr.equals("user.inode_cache")?"cache":attr.equals("user.inode_code_cache")?"code_cache":null;
            if(child!=null && node.children.containsKey(child))value=ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(node.children.get(child)).array();
            node.xattrs.put(attr,value);
        }
    }
    private static void remove(Path path)throws IOException {
        if(!Files.isSymbolicLink(path) && Files.isDirectory(path))try(DirectoryStream<Path> children=Files.newDirectoryStream(path)){for(Path child:children)remove(child);}
        Files.delete(path);
    }
}
