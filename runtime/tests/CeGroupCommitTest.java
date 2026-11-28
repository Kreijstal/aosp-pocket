// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.storage;

import java.nio.file.*;
import java.security.SecureRandom;
import java.util.*;
import java.util.concurrent.*;

/** Verifies group-commit coalescing, durability and parallel object syncs. */
public final class CeGroupCommitTest {
    private static void check(boolean ok,String message){if(!ok)throw new AssertionError(message);}

    private static void testNamespaceGroupCommit(Path vault,byte[] key)throws Exception {
        final int threads=12;
        try(EncryptedStore store=EncryptedStore.create(vault,key)) {
            final CeNamespace ns=new CeNamespace(store,true,true);
            ns.autoCommit=false;
            // Hold a commit open while 12 concurrent writers create files and call sync().
            ExecutorService pool=Executors.newFixedThreadPool(threads);
            List<Future<Void>> futures=new ArrayList<>();
            try {
                try(EncryptedStore.Commit blocker=store.beginCommit()) {
                    blocker.capture();
                    CountDownLatch created=new CountDownLatch(threads);
                    for(int i=0;i<threads;i++) {
                        final int idx=i;
                        futures.add(pool.submit(()->{
                            String path="/data/user/0/f-"+idx;
                            CeNamespace.Node n=ns.create(path,CeNamespace.REG|0600,1000,1000,new int[0],null);
                            byte[] payload=new byte[8192];
                            Arrays.fill(payload,(byte)(idx+1));
                            synchronized(ns){store.write(CeNamespace.content(n.id),0,payload,0,payload.length);ns.markDirty();}
                            created.countDown();
                            ns.sync();
                            return null;
                        }));
                    }
                    check(created.await(5,TimeUnit.SECONDS),"Writers did not reach sync()");
                    Thread.sleep(50);
                    check(store.isCommitInProgress(),"Commit should be reported in progress");
                    blocker.flush();
                }
                for(Future<Void> f:futures)f.get(10,TimeUnit.SECONDS);
            } finally {
                pool.shutdownNow();
            }
            check(!ns.hasPendingChanges(),"Pending changes remained after coalesced sync");
            long before;
            try(java.util.stream.Stream<Path> s=Files.list(vault.resolve("objects"))){before=s.count();}
            // No-op sync must not rewrite objects.
            ns.sync();
            try(java.util.stream.Stream<Path> s=Files.list(vault.resolve("objects"))){
                check(s.count()==before,"No-op sync created new objects");
            }
        }
        // Independent reopen must see all 12 files intact.
        try(EncryptedStore store=EncryptedStore.open(vault,key)) {
            CeNamespace ns=new CeNamespace(store,false,true);
            for(int i=0;i<threads;i++) {
                CeNamespace.Node n=ns.resolve("/data/user/0/f-"+i,0,0,new int[0],true);
                byte[] actual=new byte[8192],expected=new byte[8192];
                Arrays.fill(expected,(byte)(i+1));
                check(store.read(CeNamespace.content(n.id),0,actual,0,actual.length)==actual.length,"Short read for f-"+i);
                check(Arrays.equals(expected,actual),"Corrupted content for f-"+i);
            }
        }
        System.out.println("PASS: CeNamespace group-commit coalescing, no-op fast path, and durable reopen");
    }

    private static void testParallelMultiChunkCommit(Path vault,byte[] key)throws Exception {
        byte[] payload=new byte[64*EncryptedStore.CHUNK_SIZE];
        new Random(4242).nextBytes(payload);
        try(EncryptedStore store=EncryptedStore.create(vault,key)) {
            store.createFile("big");
            store.write("big",0,payload,0,payload.length);
            store.sync();
            check(!store.hasPendingChanges(),"Store still dirty after sync");
        }
        try(EncryptedStore store=EncryptedStore.open(vault,key)) {
            byte[] actual=new byte[payload.length];
            check(store.read("big",0,actual,0,actual.length)==actual.length,"Short read after parallel chunk fsync");
            check(Arrays.equals(payload,actual),"Data mismatch after parallel chunk fsync");
        }
        System.out.println("PASS: EncryptedStore parallel multi-chunk fsync and durable reopen");
    }

    public static void main(String[] args)throws Exception {
        Path parent=Files.createTempDirectory("pocket-group-commit-");
        byte[] key=new byte[32];new SecureRandom().nextBytes(key);
        try {
            testNamespaceGroupCommit(parent.resolve("ns-vault"),key);
            testParallelMultiChunkCommit(parent.resolve("chunk-vault"),key);
        } finally {
            Arrays.fill(key,(byte)0);
            try(java.util.stream.Stream<Path> paths=Files.walk(parent)) {
                for(Path p:(Iterable<Path>)paths.sorted(Comparator.reverseOrder())::iterator)Files.delete(p);
            }
        }
    }
}
