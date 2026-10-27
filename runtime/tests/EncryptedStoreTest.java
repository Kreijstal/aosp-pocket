// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.storage;

import java.io.*;
import java.nio.channels.OverlappingFileLockException;
import java.nio.file.*;
import java.security.*;
import java.util.*;

/** Behavioral tests shared by the desktop JVM and ordinary Android app. */
public final class EncryptedStoreTest {
    private static final String PRIVATE_NAME="private-customer-note-7a8d9e";
    private static final byte[] MARKER="confidential contents: storage encryption canary".getBytes(java.nio.charset.StandardCharsets.UTF_8);
    private interface Action {void run()throws Exception;}
    private static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
    private static void rejected(Action action)throws Exception {
        try {action.run();}catch(GeneralSecurityException|IOException expected){return;}
        throw new AssertionError("Unauthenticated storage was accepted");
    }
    private static void same(EncryptedStore store,String name,byte[] expected)throws Exception {
        byte[] output=new byte[expected.length+17];Arrays.fill(output,(byte)0x72);
        check(store.size(name)==expected.length,"File size mismatch");
        check(store.read(name,0,output,0,output.length)==expected.length,"Read count mismatch");
        check(Arrays.equals(expected,Arrays.copyOf(output,expected.length)),"File content mismatch");
        check(output[expected.length]==0x72,"Read modified bytes beyond EOF");
        check(store.read(name,expected.length+3,output,0,1)==0,"Read past EOF");
    }
    private static boolean contains(byte[] data,byte[] needle) {
        for(int i=0;i<=data.length-needle.length;i++) {
            int at=0;while(at<needle.length && data[i+at]==needle[at])at++;
            if(at==needle.length)return true;
        }
        return false;
    }
    private static void ciphertextOnly(Path path)throws IOException {
        if(Files.isDirectory(path)) {
            try(DirectoryStream<Path> children=Files.newDirectoryStream(path)){for(Path child:children)ciphertextOnly(child);}
        } else {
            // The backend exclusively owns this advisory-lock fd. Opening and
            // closing the same inode here could release its POSIX lock.
            if(path.getFileName().toString().equals("owner.lock"))return;
            byte[] raw=Files.readAllBytes(path);
            check(!contains(raw,MARKER),"Plaintext content persisted");
            check(!contains(raw,PRIVATE_NAME.getBytes(java.nio.charset.StandardCharsets.UTF_8)),"Plaintext logical name persisted");
        }
        check(!path.getFileName().toString().contains(PRIVATE_NAME),"Plaintext backing filename");
    }
    public static Map<String,Object> run(Path parent)throws Exception {
        Map<String,Object> result=new LinkedHashMap<>();byte[] secret=new byte[32];new SecureRandom().nextBytes(secret);
        // RFC 5869 test case 1, truncated to the 32 bytes used by this backend.
        byte[] ikm=new byte[22];Arrays.fill(ikm,(byte)0x0b);
        check(Arrays.equals(EncryptedStore.derive(ikm,hex("000102030405060708090a0b0c"),hex("f0f1f2f3f4f5f6f7f8f9")),
            hex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf")),"HKDF known-answer mismatch");
        result.put("hkdfRfc5869KnownAnswer","PASS");
        Path root=parent.resolve("random-access");EncryptedStore store=EncryptedStore.create(root,secret);
        try {
            store.createFile(PRIVATE_NAME);store.write(PRIVATE_NAME,0,MARKER,0,MARKER.length);
            store.createFile("database");byte[] expected=new byte[0];Random random=new Random(918324L);
            for(int iteration=0;iteration<180;iteration++) {
                if(iteration%6==0) {
                    int size=random.nextInt(32769);store.resize("database",size);expected=Arrays.copyOf(expected,size);
                } else {
                    int offset=random.nextInt(32768),length=1+random.nextInt(2048);byte[] input=new byte[length];random.nextBytes(input);
                    store.write("database",offset,input,0,input.length);
                    expected=Arrays.copyOf(expected,Math.max(expected.length,offset+length));System.arraycopy(input,0,expected,offset,length);
                }
                same(store,"database",expected);
                if(iteration%10==0){store.sync();store.close();store=EncryptedStore.open(root,secret);same(store,"database",expected);}
            }
            store.sync();same(store,PRIVATE_NAME,MARKER);ciphertextOnly(root);
            result.put("randomReadWriteResizeReopen","PASS (180 operations, 18 reopen cycles)");
            result.put("plaintextContentAndNamesAbsent","PASS");
            try {EncryptedStore other=EncryptedStore.open(root,secret);other.close();throw new AssertionError("Second writer accepted");}
            catch(OverlappingFileLockException|IOException expectedLock){}
            result.put("exclusiveOwner","PASS");
            byte[] committed=expected.clone();store.write("database",0,MARKER,0,MARKER.length);store.rename(PRIVATE_NAME,"uncommitted-name");
            store.createFile("uncommitted-file");store.close();
            try {store.names();throw new AssertionError("Locked store remains accessible");}catch(IllegalStateException expectedClosed){}
            store=EncryptedStore.open(root,secret);same(store,"database",committed);same(store,PRIVATE_NAME,MARKER);
            check(!store.names().contains("uncommitted-file"),"Uncommitted namespace persisted");
            result.put("uncommittedChangesDiscardedAndLockedAccessDenied","PASS");
            store.rename(PRIVATE_NAME,"renamed");store.delete("database");store.collectGarbage();store.close();
            store=EncryptedStore.open(root,secret);same(store,"renamed",MARKER);check(store.names().size()==1,"Committed delete/rename failed");
            result.put("committedRenameDeleteAndGarbageCollection","PASS");
        } finally {store.close();}
        final byte[] wrong=secret.clone();wrong[0]^=1;
        rejected(new Action(){public void run()throws Exception{try(EncryptedStore ignored=EncryptedStore.open(root,wrong)) {ignored.names();}}});
        try(EncryptedStore reopened=EncryptedStore.open(root,secret)){same(reopened,"renamed",MARKER);}
        result.put("wrongSecretRejectedWithoutDamagingStore","PASS");
        // Namespace serialization rewrites a whole logical file even when only
        // one inode changed. Its durable write amplification must stay bounded
        // by the changed data, with identical data surviving close/reopen.
        Path metadataRoot=parent.resolve("metadata-updates");byte[] metadata=new byte[1024*1024];new Random(357).nextBytes(metadata);
        byte[] pressure=new byte[3*1024*1024];new Random(358).nextBytes(pressure);
        try(EncryptedStore metadataStore=EncryptedStore.create(metadataRoot,secret)) {
            metadataStore.createFile("metadata");metadataStore.write("metadata",0,metadata,0,metadata.length);metadataStore.sync();
            int before=objects(metadataRoot);
            metadata[123456]^=1;metadataStore.write("metadata",0,metadata,0,metadata.length);metadataStore.sync();
            check(objects(metadataRoot)==before+1,"Whole-file metadata update rewrote unchanged ciphertext chunks");
            before=objects(metadataRoot);metadataStore.write("metadata",0,metadata,0,metadata.length);
            check(metadataStore.hasPendingChanges(),"Identical write lost the VFS metadata sync boundary");metadataStore.sync();
            check(objects(metadataRoot)==before,"Identical write created ciphertext objects");same(metadataStore,"metadata",metadata);
            metadataStore.createFile("cache-pressure");metadataStore.write("cache-pressure",0,pressure,0,pressure.length);metadataStore.sync();
            same(metadataStore,"cache-pressure",pressure);same(metadataStore,"metadata",metadata);same(metadataStore,"cache-pressure",pressure);
        }
        try(EncryptedStore reopened=EncryptedStore.open(metadataRoot,secret)){same(reopened,"metadata",metadata);same(reopened,"cache-pressure",pressure);}
        result.put("wholeFileMetadataWriteAmplification","PASS (one changed chunk in a 1 MiB rewrite; identical writes reuse data chunks and retain the metadata sync boundary)");
        result.put("boundedCacheEvictionAndReopen","PASS (3 MiB working set; authenticated data survives eviction and reopen)");
        Path snapshotRoot=parent.resolve("snapshot-commits");
        byte[] earlier=new byte[8192];Arrays.fill(earlier,(byte)0x41);
        try(EncryptedStore live=EncryptedStore.create(snapshotRoot,secret)) {
            live.createFile("value");live.write("value",0,earlier,0,earlier.length);live.sync();
            // Make the captured commit trigger collection. Disk references to
            // files deleted after capture must survive that collection.
            for(int i=0;i<61;i++){live.write("value",0,earlier,0,1);live.sync();}
            live.createFile("captured-only");live.write("captured-only",0,MARKER,0,MARKER.length);
            live.createFile("rewrite");live.write("rewrite",0,new byte[]{1,2,3},0,3);
            earlier[0]=0x42;live.write("value",0,earlier,0,earlier.length);
            try(EncryptedStore.Commit commit=live.beginCommit()) {
                commit.capture();
                java.util.concurrent.FutureTask<Void> writer=new java.util.concurrent.FutureTask<>(()->{
                    same(live,"value",earlier);
                    live.resize("value",0);live.resize("value",8192);
                    live.delete("captured-only");live.createFile("later-only");
                    live.write("rewrite",1,new byte[]{9},0,1);live.rename("rewrite","rewritten");
                    live.write("later-only",0,MARKER,0,MARKER.length);return null;
                });
                new Thread(writer,"SnapshotConcurrentWriter").start();
                writer.get(5,java.util.concurrent.TimeUnit.SECONDS);
                commit.flush();
                same(live,"value",new byte[8192]);check(live.hasPendingChanges(),"Concurrent writes were marked durable by an earlier snapshot");
                same(live,"rewritten",new byte[]{1,9,3});
            }
            Path frozen=parent.resolve("committed-snapshot");copy(snapshotRoot,frozen);
            try(EncryptedStore committed=EncryptedStore.open(frozen,secret)) {
                same(committed,"value",earlier);same(committed,"captured-only",MARKER);
                same(committed,"rewrite",new byte[]{1,2,3});
                check(!committed.names().contains("later-only"),"Snapshot included a later creation");
            }
            // A subsequent commit persists the later namespace and sparse
            // truncation result without resurrecting the captured data.
            live.sync();
            live.createFile("retry");live.write("retry",0,new byte[]{4},0,1);
            Files.move(snapshotRoot.resolve("state"),snapshotRoot.resolve("saved-state"));Files.createDirectory(snapshotRoot.resolve("state"));
            try {
                try(EncryptedStore.Commit commit=live.beginCommit()) {
                    commit.capture();live.write("retry",0,new byte[]{5},0,1);
                    try{commit.flush();throw new AssertionError("Commit over an invalid state target succeeded");}catch(IOException expected){}
                }
                check(live.hasPendingChanges(),"Failed commit discarded pending writes");same(live,"retry",new byte[]{5});
            }finally{Files.delete(snapshotRoot.resolve("state"));Files.move(snapshotRoot.resolve("saved-state"),snapshotRoot.resolve("state"));}
            live.sync();
        }
        try(EncryptedStore reopened=EncryptedStore.open(snapshotRoot,secret)) {
            same(reopened,"value",new byte[8192]);same(reopened,"later-only",MARKER);
            same(reopened,"rewritten",new byte[]{1,9,3});
            same(reopened,"retry",new byte[]{5});
            check(!reopened.names().contains("captured-only"),"Concurrent deletion was lost");
        }
        result.put("snapshotCommitConcurrentReadWriteTruncateDeleteAndCollection","PASS");
        result.put("failedSnapshotCommitRetainsLaterWritesForRetry","PASS");
        for(final String kind:new String[]{"key","state","chunk","swapped-chunks","missing-chunk","truncated-index"}) {
            final Path corrupted=parent.resolve("tamper-"+kind);
            try(EncryptedStore fresh=EncryptedStore.create(corrupted,secret)) {
                fresh.createFile(PRIVATE_NAME);byte[] data=new byte[8192];Arrays.fill(data,(byte)0x3d);fresh.write(PRIVATE_NAME,0,data,0,data.length);fresh.sync();
            }
            List<Path> chunks=new ArrayList<>();
            try(DirectoryStream<Path> paths=Files.newDirectoryStream(corrupted.resolve("objects"))){for(Path path:paths)chunks.add(path);}
            if(kind.equals("missing-chunk"))Files.delete(chunks.get(0));
            else if(kind.equals("truncated-index"))Files.write(corrupted.resolve("state"),new byte[7]);
            else if(kind.equals("swapped-chunks")) {
                check(chunks.size()==2,"Swap fixture chunk count");byte[] first=Files.readAllBytes(chunks.get(0)),second=Files.readAllBytes(chunks.get(1));
                Files.write(chunks.get(0),second);Files.write(chunks.get(1),first);
            } else {
                Path path=kind.equals("chunk")?chunks.get(0):corrupted.resolve(kind);byte[] raw=Files.readAllBytes(path);raw[raw.length-1]^=1;Files.write(path,raw);
            }
            rejected(new Action(){public void run()throws Exception {
                try(EncryptedStore damaged=EncryptedStore.open(corrupted,secret)){damaged.read(PRIVATE_NAME,0,new byte[8192],0,8192);}
            }});
        }
        result.put("tamperSwapMissingChunkAndTruncatedIndexRejected","PASS");
        Arrays.fill(secret,(byte)0);result.put("validation","PASS");return result;
    }
    private static byte[] hex(String value) {
        byte[] output=new byte[value.length()/2];
        for(int i=0;i<output.length;i++)output[i]=(byte)Integer.parseInt(value.substring(i*2,i*2+2),16);
        return output;
    }
    private static void copy(Path from,Path to)throws IOException {
        if(Files.isDirectory(from)){Files.createDirectory(to);try(DirectoryStream<Path> children=Files.newDirectoryStream(from)){for(Path p:children)copy(p,to.resolve(p.getFileName()));}}
        else if(from.getFileName().toString().equals("owner.lock"))Files.createFile(to);
        else Files.copy(from,to);
    }
    private static int objects(Path root)throws IOException {
        int count=0;try(DirectoryStream<Path> files=Files.newDirectoryStream(root.resolve("objects"))){for(Path ignored:files)count++;}return count;
    }
    public static void main(String[] args)throws Exception {
        if(args.length==2 && args[0].equals("crash-writer")) {
            byte[] key=new byte[32];new DataInputStream(System.in).readFully(key);
            try(EncryptedStore store=EncryptedStore.open(Paths.get(args[1]),key)) {
                System.out.println("READY");System.out.flush();byte[] data=new byte[16384];
                for(int iteration=1;;iteration++) {Arrays.fill(data,(byte)iteration);store.write("crash-file",0,data,0,data.length);store.sync();}
            }
        } else if(args.length==2 && args[0].equals("expect-locked")) {
            byte[] key=new byte[32];new DataInputStream(System.in).readFully(key);
            try(EncryptedStore store=EncryptedStore.open(Paths.get(args[1]),key)){store.names();throw new AssertionError("Cross-process writer lock lost");}
            catch(IOException expected){System.out.println("PASS: competing process denied");}
        } else if(args.length==2 && args[0].equals("crash-create")) {
            byte[] key=new byte[32];new DataInputStream(System.in).readFully(key);
            try(EncryptedStore store=EncryptedStore.create(Paths.get(args[1]),key)) {
                store.createFile("crash-file");store.resize("crash-file",16384);store.sync();
            }
        } else if(args.length==2 && args[0].equals("crash-verify")) {
            byte[] key=new byte[32];new DataInputStream(System.in).readFully(key);
            try(EncryptedStore store=EncryptedStore.open(Paths.get(args[1]),key)) {
                byte[] output=new byte[16384];check(store.read("crash-file",0,output,0,output.length)==output.length,"Crash lost file");
                for(byte b:output)check(b==output[0],"Crash exposed a torn commit");
            }
            System.out.println("PASS: committed snapshot survived SIGKILL");
        } else {
            Path parent=Files.createTempDirectory("pocket-ce-test-");
            try {System.out.println(run(parent));}finally{remove(parent);}
        }
    }
    private static void remove(Path path)throws IOException {
        if(Files.isDirectory(path))try(DirectoryStream<Path> children=Files.newDirectoryStream(path)){for(Path child:children)remove(child);}
        Files.delete(path);
    }
}
