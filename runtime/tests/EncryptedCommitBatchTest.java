// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.storage;

import java.io.IOException;
import java.nio.file.*;
import java.security.SecureRandom;
import java.util.*;

/** Real durable commits on fresh synthetic data, shared by JVM and Android. */
public final class EncryptedCommitBatchTest {
    public static Map<String,Object> run(Path parent)throws Exception {
        Path root=parent.resolve("vault");
        if(Files.exists(root))throw new IOException("Preserve previous benchmark data; use a fresh directory");
        byte[] secret=new byte[64],data=new byte[32*EncryptedStore.CHUNK_SIZE];
        new SecureRandom().nextBytes(secret);
        List<Long> elapsed=new ArrayList<>();
        try {
            try(EncryptedStore store=EncryptedStore.create(root,secret)){store.createFile("value");store.sync();}
            for(int round=0;round<4;round++) {
                Arrays.fill(data,(byte)(37+round));
                try(EncryptedStore store=EncryptedStore.open(root,secret)) {
                    store.write("value",0,data,0,data.length);
                    long start=System.nanoTime();store.sync();elapsed.add(System.nanoTime()-start);
                    if(store.hasPendingChanges())throw new AssertionError("Successful sync left pending changes");
                }
                try(EncryptedStore store=EncryptedStore.open(root,secret)) {
                    byte[] actual=new byte[data.length];
                    if(store.read("value",0,actual,0,actual.length)!=actual.length || !Arrays.equals(actual,data))
                        throw new AssertionError("Durable commit failed independent reopen");
                    Arrays.fill(actual,(byte)0);
                }
            }
            Map<String,Object> result=new LinkedHashMap<>();
            result.put("validation","PASS");result.put("rounds",4);result.put("chunksPerRound",32);
            result.put("bytesPerRound",data.length);result.put("commitNanos",elapsed);
            result.put("independentReopens",4);result.put("syntheticFreshStore",true);
            return result;
        } finally {Arrays.fill(secret,(byte)0);Arrays.fill(data,(byte)0);}
    }
    public static void main(String[] args)throws Exception {
        if(args.length!=1)throw new IllegalArgumentException("Fresh test directory required");
        Path parent=Paths.get(args[0]);Files.createDirectory(parent);
        System.out.println(run(parent));
    }
}
