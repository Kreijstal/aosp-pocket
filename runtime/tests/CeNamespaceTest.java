// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.storage;
import java.nio.file.*;
import java.util.*;
public final class CeNamespaceTest {
    private static final int[] NONE={};
    private static void check(boolean value){if(!value)throw new AssertionError("CE namespace invariant");}
    public static void main(String[] args)throws Exception {
        Path parent=Files.createTempDirectory("pocket-namespace-");byte[] secret=new byte[64];new java.security.SecureRandom().nextBytes(secret);
        int file;
        try(EncryptedStore store=EncryptedStore.create(parent.resolve("vault"),secret)) {
            CeNamespace fs=new CeNamespace(store,true);
            CeNamespace.Node dir=fs.create("/data/user/0/app",CeNamespace.DIR|0700,0,0,NONE,null);dir.uid=dir.gid=12345;
            CeNamespace.Node n=fs.create("/data/data/app/value",CeNamespace.REG|0600,12345,12345,NONE,null);file=n.id;
            store.write(CeNamespace.content(file),0,new byte[]{1,2,3},0,3);
            check(fs.resolve("/data/user/0/app/./value",12345,12345,NONE,true).id==file);
            try{fs.resolve("/data/user/0/app/value",12346,12346,NONE,true);throw new AssertionError("Traversal permission bypass");}
            catch(CeNamespace.FsException expected){check(expected.errno==13);}
            fs.create("/data/user/0/app/link",CeNamespace.LNK|0777,12345,12345,NONE,"value");
            check(fs.resolve("/data/user/0/app/link",12345,12345,NONE,true).id==file);
            check(fs.resolve("/data/user/0/app/link",12345,12345,NONE,false).symlink());
            fs.create("/data/user/0/app/outside",CeNamespace.LNK|0777,12345,12345,NONE,"/data/user_de/0");
            try{fs.resolve("/data/user/0/app/outside",12345,12345,NONE,true);throw new AssertionError("External symlink swallowed");}
            catch(CeNamespace.Outside expected){check(expected.path.equals("/data/user_de/0"));}
            fs.link("/data/user/0/app/value","/data/user/0/app/second",12345,12345,NONE);
            check(n.links==2);fs.remove("/data/user/0/app/value",false,12345,12345,NONE);check(n.links==1);
            fs.rename("/data/user/0/app/second","/data/user/0/app/renamed",false,12345,12345,NONE);
            check(fs.resolve("/data/user/0/app/renamed",12345,12345,NONE,true).id==file);
            fs.create("/data/user/0/app/child",CeNamespace.DIR|0700,12345,12345,NONE,null);
            try{fs.rename("/data/user/0/app","/data/user/0/app/child/cycle",false,0,0,NONE);throw new AssertionError("Directory cycle accepted");}
            catch(CeNamespace.FsException expected){check(expected.errno==22);}
            check(fs.resolve("/data/user/0/app/child/..",12345,12345,NONE,true).id==dir.id);
            fs.sync();
        }
        try(EncryptedStore store=EncryptedStore.open(parent.resolve("vault"),secret)) {
            CeNamespace fs=new CeNamespace(store,false);CeNamespace.Node n=fs.resolve("/data/user/0/app/renamed",12345,12345,NONE,true);
            check(n.id==file && n.uid==12345 && fs.size(n)==3);
            byte[] bytes=new byte[3];check(store.read(CeNamespace.content(file),0,bytes,0,3)==3);check(Arrays.equals(bytes,new byte[]{1,2,3}));
            fs.remove("/data/user/0/app/renamed",false,12345,12345,NONE);check(n.links==0);
            check(store.read(CeNamespace.content(file),0,bytes,0,3)==3);fs.sync();
        }
        // Production buffers metadata, but explicit sync is a durability barrier.
        try(EncryptedStore store=EncryptedStore.open(parent.resolve("vault"),secret)) {
            CeNamespace fs=new CeNamespace(store,false);fs.autoCommit=false;check(!fs.hasPendingChanges());
            fs.create("/data/user/0/batched",CeNamespace.DIR|0700,0,0,NONE,null);
            check(fs.hasPendingChanges());check(!store.hasPendingChanges());
            CeNamespace.Node n=fs.create("/data/user/0/batched/value",CeNamespace.REG|0600,0,0,NONE,null);
            store.write(CeNamespace.content(n.id),0,new byte[]{42},0,1);
            fs.sync();check(!fs.hasPendingChanges());
            n.mode=CeNamespace.REG|0640;fs.markDirty();check(fs.hasPendingChanges());
            fs.sync();check(!fs.hasPendingChanges());
            fs.create("/data/user/0/uncommitted",CeNamespace.DIR|0700,0,0,NONE,null);
            check(fs.hasPendingChanges());
        }
        try(EncryptedStore store=EncryptedStore.open(parent.resolve("vault"),secret)) {
            CeNamespace fs=new CeNamespace(store,false);
            CeNamespace.Node n=fs.resolve("/data/user/0/batched/value",0,0,NONE,true);
            check(n.mode==(CeNamespace.REG|0640));byte[] data=new byte[1];
            check(store.read(CeNamespace.content(n.id),0,data,0,1)==1 && data[0]==42);
            try{fs.resolve("/data/user/0/uncommitted",0,0,NONE,true);throw new AssertionError("Uncommitted metadata survived close");}
            catch(CeNamespace.FsException expected){check(expected.errno==2);}
        }
        try(EncryptedStore store=EncryptedStore.open(parent.resolve("vault"),secret)) {
            CeNamespace fs=new CeNamespace(store,false);
            java.util.concurrent.FutureTask<Void> writer=new java.util.concurrent.FutureTask<>(()->{
                for(int i=0;i<40;i++)fs.create("/data/user/0/concurrent-"+i,CeNamespace.DIR|0700,0,0,NONE,null);
                return null;
            });
            java.util.concurrent.FutureTask<Void> committer=new java.util.concurrent.FutureTask<>(()->{for(int i=0;i<40;i++)fs.sync();return null;});
            Thread a=new Thread(writer,"NamespaceWriter"),b=new Thread(committer,"NamespaceCommitter");a.setDaemon(true);b.setDaemon(true);a.start();b.start();
            writer.get(15,java.util.concurrent.TimeUnit.SECONDS);committer.get(15,java.util.concurrent.TimeUnit.SECONDS);fs.sync();
            CeNamespace.Node pressure=fs.create("/data/user/0/cache-pressure",CeNamespace.REG|0600,0,0,NONE,null);
            byte[] data=new byte[3*1024*1024];new Random(751).nextBytes(data);
            store.write(CeNamespace.content(pressure.id),0,data,0,data.length);fs.markDirty();fs.sync();
            byte[] read=new byte[data.length];check(store.read(CeNamespace.content(pressure.id),0,read,0,read.length)==read.length && Arrays.equals(data,read));
            CeNamespace.Node changed=fs.resolve("/data/user/0/batched/value",0,0,NONE,true);
            changed.mode^=0111;fs.markDirty();fs.sync();changed.mode^=0111;fs.markDirty();fs.sync();
        }
        try(EncryptedStore store=EncryptedStore.open(parent.resolve("vault"),secret)) {
            CeNamespace fs=new CeNamespace(store,false);
            for(int i=0;i<40;i++)check(fs.resolve("/data/user/0/concurrent-"+i,0,0,NONE,true).directory());
            check(fs.size(fs.resolve("/data/user/0/cache-pressure",0,0,NONE,true))==3*1024*1024);
            CeNamespace.Node n=fs.resolve("/data/user/0/batched/value",0,0,NONE,true);int committedMode=n.mode;
            n.mode=CeNamespace.REG|0600;fs.markDirty();
            Path state=parent.resolve("vault/state"),saved=parent.resolve("vault/saved-state");Files.move(state,saved);Files.createDirectory(state);
            try{try{fs.sync();throw new AssertionError("Invalid commit target accepted");}catch(java.io.IOException expected){}check(fs.hasPendingChanges());}
            finally{Files.delete(state);Files.move(saved,state);}
            // Return to the previous committed bytes after a failed commit.
            // Comparing against only the committed hash used to leave the
            // failed intermediate index in memory and publish the wrong mode.
            n.mode=committedMode;fs.markDirty();fs.sync();check(!fs.hasPendingChanges());
        }
        try(EncryptedStore store=EncryptedStore.open(parent.resolve("vault"),secret)) {
            CeNamespace fs=new CeNamespace(store,false);
            check(fs.resolve("/data/user/0/batched/value",0,0,NONE,true).mode==(CeNamespace.REG|0640));
        }
        Arrays.fill(secret,(byte)0);remove(parent);
        System.out.println("PASS: encrypted namespace persistence, buffered metadata, explicit sync durability, traversal permissions, symlinks, hard links, rename/unlink and directory-cycle rejection");
    }
    private static void remove(Path p)throws Exception {
        if(Files.isDirectory(p))try(DirectoryStream<Path> children=Files.newDirectoryStream(p)){for(Path c:children)remove(c);}
        Files.delete(p);
    }
}
