// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.storage;
import java.nio.file.*;
import java.util.*;
/** Replacing a complete, changed metadata chunk must not reread its old disk bytes. */
public final class CeSnapshotNoReadTest {
 private static void contents(EncryptedStore store,byte[] expected)throws Exception {
  byte[] actual=new byte[expected.length];
  if(store.size("snapshot")!=expected.length || store.read("snapshot",0,actual,0,actual.length)!=actual.length
    || !Arrays.equals(expected,actual))throw new AssertionError("snapshot contents changed");
 }
 private static void replacements(Path vault,byte[] key)throws Exception {
  int chunk=EncryptedStore.CHUNK_SIZE;byte[] expected=new byte[chunk*2+17];new Random(893).nextBytes(expected);
  try(EncryptedStore store=EncryptedStore.create(vault,key)) {
   store.createFile("snapshot");store.replaceSnapshot("snapshot",expected,new boolean[]{true,true,true});store.sync();
  }
  try(EncryptedStore store=EncryptedStore.open(vault,key)) {
   contents(store,expected);long before;
   try(java.util.stream.Stream<Path> paths=Files.list(vault.resolve("objects"))){before=paths.count();}
   expected[chunk+3]^=7;
   store.replaceSnapshot("snapshot",expected,new boolean[]{false,true,false});store.sync();contents(store,expected);
   try(java.util.stream.Stream<Path> paths=Files.list(vault.resolve("objects"))){
    if(paths.count()!=before+1)throw new AssertionError("unchanged snapshot chunks rewritten");
   }
   expected=Arrays.copyOf(expected,chunk+5);
   store.replaceSnapshot("snapshot",expected,new boolean[]{false,true});store.sync();contents(store,expected);
  }
  try(EncryptedStore store=EncryptedStore.open(vault,key)) {
   contents(store,expected);expected=Arrays.copyOf(expected,chunk*2+9);
   store.replaceSnapshot("snapshot",expected,new boolean[]{false,true,true});store.sync();contents(store,expected);
   try {store.replaceSnapshot("snapshot",Arrays.copyOf(expected,expected.length+1),new boolean[]{false,false,false});throw new AssertionError("unchanged growth accepted");}
   catch(IllegalArgumentException correct){contents(store,expected);}
   try {store.replaceSnapshot("snapshot",expected,new boolean[]{true});throw new AssertionError("bad bitmap accepted");}
   catch(IllegalArgumentException correct){contents(store,expected);}
  }
  try(EncryptedStore store=EncryptedStore.open(vault,key)){contents(store,expected);}
  System.out.println("PASS: snapshot chunk reuse, partial-tail shrink/growth, zero fill, invalid-range rejection and durable reopen");
 }
 public static void main(String[] args)throws Exception {
  Path parent=Files.createTempDirectory("pocket-snapshot-no-read-");Path vault=parent.resolve("vault");
  byte[] key=new byte[32];new java.security.SecureRandom().nextBytes(key);
  try {
   try(EncryptedStore store=EncryptedStore.create(vault,key)) {
    CeNamespace ns=new CeNamespace(store,true);ns.autoCommit=false;
    Path old;
    try(java.util.stream.Stream<Path> objects=Files.list(vault.resolve("objects"))){old=objects.findFirst().get();}
    // Evict the small namespace chunk by reading a larger ordinary file.
    byte[] pressure=new byte[3*1024*1024];new Random(891).nextBytes(pressure);
    store.createFile("cache-pressure");store.write("cache-pressure",0,pressure,0,pressure.length);store.sync();
    byte[] actual=new byte[pressure.length];store.read("cache-pressure",0,actual,0,actual.length);
    if(!Arrays.equals(pressure,actual))throw new AssertionError("cache-pressure data changed");
    Files.delete(old);
    ns.node(3).mtime=1794916000000L;ns.markDirty();
    ns.sync(); // Authoritative new metadata replaces the complete old chunk.
   }
   try(EncryptedStore store=EncryptedStore.open(vault,key)) {
    CeNamespace ns=new CeNamespace(store,false);
    if(ns.node(3).mtime!=1794916000000L)throw new AssertionError("new metadata not durable");
   }
   System.out.println("PASS: cold namespace snapshot replaces old chunk without a disk read; durable reopen verified");
   replacements(parent.resolve("replacement-vault"),key);
  } finally {
   Arrays.fill(key,(byte)0);
   try(java.util.stream.Stream<Path> paths=Files.walk(parent)){
    for(Path p:(Iterable<Path>)paths.sorted(Comparator.reverseOrder())::iterator)Files.delete(p);
   }
  }
 }
}
