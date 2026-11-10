// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.storage;
import java.nio.file.*;
import java.nio.ByteBuffer;
import java.util.*;

/** Verify12L's extra CE root without reinterpreting existing legacy inodes. */
public final class CeNamespace32Test {
    private static final int[] NONE={};
    private static void check(boolean value){if(!value)throw new AssertionError();}
    private static int version(EncryptedStore store)throws Exception {
        byte[] header=new byte[4];check(store.read("namespace-v1",0,header,0,4)==4);return ByteBuffer.wrap(header).getInt();
    }
    private static void outside(CeNamespace fs,String path,String expected)throws Exception {
        try{fs.resolve(path,0,0,NONE,true);throw new AssertionError("Root traversal swallowed");}
        catch(CeNamespace.Outside exit){check(exit.path.equals(expected));}
    }
    public static void main(String[] args)throws Exception {
        Path dir=Files.createTempDirectory("pocket-ce32-");byte[] secret=new byte[64];new java.security.SecureRandom().nextBytes(secret);
        byte[] payload="private vendor credential32".getBytes("UTF-8");int vendorFile;
        try {
            try(EncryptedStore store=EncryptedStore.create(dir.resolve("legacy"),secret)) {
                CeNamespace fs=new CeNamespace(store,true);check(version(store)==2);
                check(fs.create("/data/user/0/legacy-file7",CeNamespace.REG|0600,0,0,NONE,null).id==7);fs.sync();
            }
            try(EncryptedStore store=EncryptedStore.open(dir.resolve("legacy"),secret)) {
                CeNamespace fs=new CeNamespace(store,false);check(fs.roots().length==4);
                check(!fs.node(7).directory() && fs.rootId("/data/vendor_ce/0")==0);
                outside(fs,"/data/vendor_ce/0","/data/vendor_ce/0");fs.sync();check(version(store)==2);
            }
            try(EncryptedStore store=EncryptedStore.create(dir.resolve("api32"),secret)) {
                CeNamespace fs=new CeNamespace(store,true,true);check(version(store)==3 && fs.roots().length==5);
                for(String root:CeNamespace.API32_ROOTS)check(fs.resolve(root,0,0,NONE,true).directory());
                CeNamespace.Node vendor=fs.resolve("/data/vendor_ce/0",0,0,NONE,true);check(vendor.id==7 && vendor.uid==0 && vendor.gid==0);
                byte[] label="inherited-host-label\0".getBytes("UTF-8");vendor.xattrs.put("security.selinux",label);
                CeNamespace.Node file=fs.create("/data/vendor_ce/0/vendor-key",CeNamespace.REG|0600,0,0,NONE,null);vendorFile=file.id;check(vendorFile>=8);
                check(Arrays.equals(file.xattrs.get("security.selinux"),label) && file.xattrs.get("security.selinux")!=label);
                check(fs.path(file.id).equals("/data/vendor_ce/0/vendor-key"));
                store.write(CeNamespace.content(file.id),0,payload,0,payload.length);fs.sync();
                outside(fs,"/data/vendor_ce/0/..","/data/vendor_ce");
                fs.create("/data/user/0/vendor-link",CeNamespace.LNK|0777,0,0,NONE,"/data/vendor_ce/0/vendor-key");
                check(fs.resolve("/data/user/0/vendor-link",0,0,NONE,true).id==vendorFile);
                try{fs.resolve("/data/vendor_ce/0/vendor-key",10107,10107,NONE,true);CeNamespace.access(file,10107,10107,NONE,4);throw new AssertionError("Private vendor file readable by app");}
                catch(CeNamespace.FsException denied){check(denied.errno==13);}
            }
            try(EncryptedStore store=EncryptedStore.open(dir.resolve("api32"),secret)) {
                CeNamespace fs=new CeNamespace(store,false);check(version(store)==3 && fs.roots().length==5);
                check(fs.path(7).equals("/data/vendor_ce/0"));
                CeNamespace.Node file=fs.resolve("/data/vendor_ce/0/vendor-key",0,0,NONE,true);check(file.id==vendorFile);
                check(Arrays.equals(file.xattrs.get("security.selinux"),"inherited-host-label\0".getBytes("UTF-8")));
                byte[] result=new byte[payload.length];check(store.read(CeNamespace.content(file.id),0,result,0,result.length)==result.length && Arrays.equals(result,payload));
            }
            try(java.util.stream.Stream<Path> files=Files.walk(dir.resolve("api32"))) {
                for(Iterator<Path> it=files.filter(Files::isRegularFile).iterator();it.hasNext();) {
                    byte[] bytes=Files.readAllBytes(it.next());check(!new String(bytes,java.nio.charset.StandardCharsets.ISO_8859_1).contains("private vendor credential32"));
                }
            }
            System.out.println("CE_NAMESPACE32_PASS legacy_format2_inode7=preserved api32_format3_roots=5 vendor_content=persistent_encrypted app_access=denied traversal=correct");
        }finally{Arrays.fill(secret,(byte)0);try(java.util.stream.Stream<Path> paths=Files.walk(dir)){for(Path path:(Iterable<Path>)paths.sorted(Comparator.reverseOrder())::iterator)Files.delete(path);}}
    }
}
