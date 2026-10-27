// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.storage;

import java.io.*;
import java.nio.ByteBuffer;
import java.nio.channels.*;
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermissions;
import java.security.*;
import java.util.*;
import java.util.concurrent.locks.ReentrantLock;
import javax.crypto.*;
import javax.crypto.spec.*;

/**
 * Staged CE storage backend, not a mounted filesystem. All persistent content
 * and logical names are authenticated ciphertext. Callers must supply a
 * high-entropy Android synthetic-password disk secret, NEVER a raw password,
 * PIN, or an unstretched credential hash. Guest/vold integration is separate.
 *
 * Immutable chunks plus an atomically replaced encrypted index make sync()
 * the commit boundary. close() discards uncommitted writes and releases keys.
 * This software backend cannot prevent rollback by the host or protect keys
 * from a compromised host or an unlocked guest. Java providers may retain
 * internal key copies until GC; clearing arrays is only best effort.
 */
public final class EncryptedStore implements AutoCloseable {
    public static final int CHUNK_SIZE=4096;
    private static final int CACHED_CHUNKS=512;
    private static final int MAX_INDEX=16*1024*1024,MAX_FILES=10000,MAX_CHUNKS=500000;
    private static final long MAX_SIZE=1L<<40;
    private static final byte[] MAGIC={'P','C','K','C','E','0','0','1'};
    private static final SecureRandom RANDOM=new SecureRandom();
    private static final Set<Path> OPEN=new HashSet<>();
    private final Path root,objects;
    private final FileChannel owner;
    private final FileLock ownerLock;
    private final TreeMap<String,Entry> entries=new TreeMap<>();
    // Immutable ciphertext objects are authenticated once before entering this
    // bounded unlocked-memory cache. Never persist plaintext cache contents.
    private final LinkedHashMap<String,byte[]> readCache=new LinkedHashMap<>(16,0.75f,true);
    private byte[] storeId,indexKey,contentKey;
    private boolean closed;
    private boolean modified=true;
    private long revision;
    private final ReentrantLock commits=new ReentrantLock(true);
    private Set<String> committedObjects=new HashSet<>();
    private int commitsSinceCollection;

    private static final class Entry {
        String id;long size;
        Entry original;
        TreeMap<Long,String> blocks=new TreeMap<>();
        TreeMap<Long,byte[]> dirty=new TreeMap<>();
        Entry(String id){this.id=id;}
    }

    private EncryptedStore(Path directory)throws IOException {
        root=directory.toRealPath();objects=root.resolve("objects");
        // POSIX locks can be released by closing ANY fd for the same inode in
        // this process. Reject a second local owner before opening its lock
        // file, rather than opening and then closing an overlapping channel.
        synchronized(OPEN) {
            if(!OPEN.add(root))throw new IOException("Encrypted store already open");
            FileChannel channel=null;
            try {
                channel=FileChannel.open(root.resolve("owner.lock"),EnumSet.of(StandardOpenOption.CREATE,StandardOpenOption.WRITE),
                    PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rw-------")));
                FileLock lock=channel.tryLock();if(lock==null)throw new IOException("Encrypted store already open");
                owner=channel;ownerLock=lock;
            } catch(IOException|RuntimeException error) {
                OPEN.remove(root);if(channel!=null)channel.close();throw error;
            }
        }
    }

    public static EncryptedStore create(Path root,byte[] syntheticSecret)throws IOException,GeneralSecurityException {
        requireSecret(syntheticSecret);
        Files.createDirectory(root,PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rwx------")));
        EncryptedStore store=new EncryptedStore(root);
        try {
            Files.createDirectory(store.objects,PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rwx------")));
            store.storeId=random(16);byte[] salt=random(32),master=random(32),wrap=null;
            try {
                wrap=derive(syntheticSecret,salt,concat("Pocket CE wrapping v1",store.storeId));
                byte[] header=concat(MAGIC,store.storeId,salt);
                writeNew(root.resolve("key"),concat(header,seal(wrap,header,master)));
                store.installKeys(master);store.sync();syncDirectory(root.toAbsolutePath().getParent());
            } finally {wipe(master);wipe(wrap);}
            return store;
        } catch(IOException|GeneralSecurityException|RuntimeException error){store.close();throw error;}
    }

    public static EncryptedStore open(Path root,byte[] syntheticSecret)throws IOException,GeneralSecurityException {
        requireSecret(syntheticSecret);
        EncryptedStore store=new EncryptedStore(root);
        try {
            byte[] keyFile=readBounded(root.resolve("key"),116);
            if(keyFile.length!=116 || !Arrays.equals(MAGIC,Arrays.copyOf(keyFile,8)))throw new IOException("Unknown encrypted-store format");
            store.storeId=Arrays.copyOfRange(keyFile,8,24);
            byte[] salt=Arrays.copyOfRange(keyFile,24,56),wrap=null,master=null;
            try {
                wrap=derive(syntheticSecret,salt,concat("Pocket CE wrapping v1",store.storeId));
                master=unseal(wrap,Arrays.copyOf(keyFile,56),Arrays.copyOfRange(keyFile,56,keyFile.length));
                if(master.length!=32)throw new IOException("Invalid encrypted-store key");
                store.installKeys(master);
            } finally {wipe(wrap);wipe(master);}
            byte[] index=unseal(store.indexKey,store.context("index"),readBounded(root.resolve("state"),MAX_INDEX+28));
            try {store.decode(index);}finally{wipe(index);}
            store.modified=false;
            for(Entry e:store.entries.values())store.committedObjects.addAll(e.blocks.values());
            return store;
        } catch(IOException|GeneralSecurityException|RuntimeException error){store.close();throw error;}
    }

    private void installKeys(byte[] master)throws GeneralSecurityException {
        indexKey=derive(master,storeId,utf8("Pocket CE index v1"));
        contentKey=derive(master,storeId,utf8("Pocket CE content v1"));
    }
    private static void requireSecret(byte[] secret) {
        if(secret==null || secret.length<32)throw new IllegalArgumentException("A high-entropy synthetic-password disk secret is required");
    }
    private void ready(){if(closed)throw new IllegalStateException("Encrypted store is locked");}
    private Entry file(String name)throws IOException {ready();Entry e=entries.get(name);if(e==null)throw new FileNotFoundException(name);return e;}
    private static void name(String name) {
        if(name==null || name.isEmpty() || name.length()>4096 || name.indexOf('\0')>=0)
            throw new IllegalArgumentException("Invalid logical file name");
    }
    public synchronized void createFile(String name)throws IOException {
        ready();name(name);if(entries.containsKey(name))throw new FileAlreadyExistsException(name);
        if(entries.size()>=MAX_FILES)throw new IOException("Encrypted-store file limit");
        entries.put(name,new Entry(id()));modified=true;revision++;
    }
    public synchronized List<String> names(){ready();return new ArrayList<>(entries.keySet());}
    public synchronized long size(String name)throws IOException{return file(name).size;}
    public synchronized void rename(String from,String to)throws IOException {
        Entry e=file(from);name(to);if(entries.containsKey(to))throw new FileAlreadyExistsException(to);
        entries.remove(from);entries.put(to,e);modified=true;revision++;
    }
    public synchronized void delete(String name)throws IOException {
        Entry e=file(name);for(byte[] b:e.dirty.values())wipe(b);entries.remove(name);modified=true;revision++;
    }
    private static void range(long offset,byte[] bytes,int start,int length) {
        if(offset<0 || offset>MAX_SIZE || length<0 || start<0 || start>bytes.length-length || length>MAX_SIZE-offset)
            throw new IndexOutOfBoundsException();
    }
    public synchronized int read(String name,long offset,byte[] output,int start,int length)throws IOException,GeneralSecurityException {
        Entry e=file(name);range(offset,output,start,length);
        int count=(int)Math.min(length,Math.max(0,e.size-offset));
        for(int done=0;done<count;) {
            long block=(offset+done)/CHUNK_SIZE;int at=(int)((offset+done)%CHUNK_SIZE),n=Math.min(count-done,CHUNK_SIZE-at);
            byte[] value=block(e,block);
            try {System.arraycopy(value,at,output,start+done,n);}finally{wipe(value);}
            done+=n;
        }
        return count;
    }
    public synchronized void write(String name,long offset,byte[] input,int start,int length)throws IOException,GeneralSecurityException {
        Entry e=file(name);range(offset,input,start,length);if(length==0)return;
        // A successful write still needs a sync boundary: VFS callers also
        // update inode timestamps even when the payload bytes are identical.
        // Reuse unchanged data chunks without losing that metadata flush.
        modified=true;revision++;
        for(int done=0;done<length;) {
            long block=(offset+done)/CHUNK_SIZE;int at=(int)((offset+done)%CHUNK_SIZE),n=Math.min(length-done,CHUNK_SIZE-at);
            byte[] value=e.dirty.get(block);
            boolean owned=value!=null;
            if(!owned)value=block(e,block);
            boolean changed=false;
            for(int i=0;i<n;i++)if(value[at+i]!=input[start+done+i]){changed=true;break;}
            if(changed) {
                System.arraycopy(input,start+done,value,at,n);
                if(!owned)e.dirty.put(block,value);
                modified=true;
            }else if(!owned)wipe(value);
            done+=n;
            if(offset+done>e.size){e.size=offset+done;modified=true;}
        }
    }
    public synchronized void resize(String name,long size)throws IOException,GeneralSecurityException {
        Entry e=file(name);if(size<0 || size>MAX_SIZE)throw new IllegalArgumentException("Invalid size");if(size==e.size)return;modified=true;revision++;
        if(size<e.size) {
            long end=(size+CHUNK_SIZE-1)/CHUNK_SIZE;
            e.blocks.tailMap(end).clear();
            for(byte[] value:e.dirty.tailMap(end).values())wipe(value);
            e.dirty.tailMap(end).clear();
            if(size%CHUNK_SIZE!=0) {
                long block=size/CHUNK_SIZE;byte[] value=e.dirty.get(block);
                if(value==null){value=block(e,block);e.dirty.put(block,value);}
                Arrays.fill(value,(int)(size%CHUNK_SIZE),CHUNK_SIZE,(byte)0);
            }
        }
        e.size=size;
    }
    private byte[] block(Entry e,long index)throws IOException,GeneralSecurityException {
        byte[] dirty=e.dirty.get(index);if(dirty!=null)return dirty.clone();
        String object=e.blocks.get(index);if(object==null)return new byte[CHUNK_SIZE];
        byte[] cached=readCache.get(object);if(cached!=null)return cached.clone();
        byte[] result=unseal(contentKey,blockContext(e,index,object),readBounded(objects.resolve(object),CHUNK_SIZE+28));
        if(result.length!=CHUNK_SIZE){wipe(result);throw new IOException("Invalid encrypted chunk size");}
        if(readCache.size()>=CACHED_CHUNKS) {
            Iterator<Map.Entry<String,byte[]>> oldest=readCache.entrySet().iterator();
            Map.Entry<String,byte[]> item=oldest.next();wipe(item.getValue());oldest.remove();
        }
        readCache.put(object,result);return result.clone();
    }
    private byte[] blockContext(Entry e,long index,String object)throws IOException {
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();DataOutputStream out=new DataOutputStream(bytes);
        out.write(context("chunk"));out.writeUTF(e.id);out.writeLong(index);out.writeUTF(object);return bytes.toByteArray();
    }

    /** Reserve a commit without blocking ordinary reads or writes. */
    public Commit beginCommit(){return new Commit();}
    public final class Commit implements AutoCloseable {
        private final Thread thread=Thread.currentThread();
        private TreeMap<String,Entry> captured;
        private long capturedRevision;
        private boolean pending,prepared,flushed,released;
        private Commit(){if(commits.isHeldByCurrentThread())throw new IllegalStateException("Nested CE commit");commits.lock();try{synchronized(EncryptedStore.this){ready();}}catch(RuntimeException error){commits.unlock();throw error;}}
        private void owned(){if(released || Thread.currentThread()!=thread)throw new IllegalStateException("Invalid CE commit owner");}
        /** Call while the namespace snapshot is still locked. */
        public void capture() {
            owned();if(prepared)throw new IllegalStateException("CE commit already captured");prepared=true;
            synchronized(EncryptedStore.this) {
                ready();capturedRevision=revision;pending=modified;if(!pending)return;
                captured=new TreeMap<>();
                for(Map.Entry<String,Entry> item:entries.entrySet()) {
                    Entry source=item.getValue(),e=new Entry(source.id);e.original=source;e.size=source.size;
                    e.blocks.putAll(source.blocks);captured.put(item.getKey(),e);
                    for(Map.Entry<Long,byte[]> block:source.dirty.entrySet())e.dirty.put(block.getKey(),block.getValue().clone());
                }
            }
        }
        /** Disk I/O uses the immutable snapshot, without filesystem monitors. */
        public void flush()throws IOException,GeneralSecurityException {
            owned();if(!prepared || flushed)throw new IllegalStateException("Invalid CE commit state");flushed=true;
            if(!pending)return;
            for(Entry e:captured.values())for(Map.Entry<Long,byte[]> b:e.dirty.entrySet()) {
                String object=id();writeNew(objects.resolve(object),seal(contentKey,blockContext(e,b.getKey(),object),b.getValue()));
                e.blocks.put(b.getKey(),object);
            }
            syncDirectory(objects);
            byte[] index=encode(captured);Path temporary=root.resolve("state-"+id());
            try {
                writeNew(temporary,seal(indexKey,context("index"),index));
                Files.move(temporary,root.resolve("state"),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
                syncDirectory(root);
            }finally{wipe(index);Files.deleteIfExists(temporary);}
            boolean collect;
            synchronized(EncryptedStore.this) {
                Set<Entry> live=Collections.newSetFromMap(new IdentityHashMap<Entry,Boolean>());live.addAll(entries.values());
                Set<String> committed=new HashSet<>();
                for(Entry e:captured.values()) {
                    committed.addAll(e.blocks.values());Entry current=e.original;if(!live.contains(current))continue;
                    for(Map.Entry<Long,byte[]> b:e.dirty.entrySet()) {
                        byte[] latest=current.dirty.get(b.getKey());
                        // A concurrent truncate/grow may have removed this
                        // block. Never resurrect its previous contents.
                        if(latest==null || b.getKey()>=(current.size+CHUNK_SIZE-1)/CHUNK_SIZE)continue;
                        current.blocks.put(b.getKey(),e.blocks.get(b.getKey()));
                        if(Arrays.equals(latest,b.getValue())){current.dirty.remove(b.getKey());wipe(latest);}
                    }
                }
                committedObjects=committed;modified=revision!=capturedRevision;
                collect=++commitsSinceCollection>=64;
            }
            if(collect){collectUnreferenced();commitsSinceCollection=0;}
        }
        @Override public void close() {
            if(released)return;owned();
            try{if(captured!=null)for(Entry e:captured.values())for(byte[] bytes:e.dirty.values())wipe(bytes);}
            finally{released=true;commits.unlock();}
        }
    }
    /** Durably commit all file contents and names as one transaction. */
    public void sync()throws IOException,GeneralSecurityException {
        try(Commit commit=beginCommit()){commit.capture();commit.flush();}
    }
    public synchronized boolean hasPendingChanges(){ready();return modified;}

    /** Reclaim unreachable ciphertext only after committing the current state. */
    public void collectGarbage()throws IOException,GeneralSecurityException {
        try(Commit commit=beginCommit()){commit.capture();commit.flush();collectUnreferenced();commitsSinceCollection=0;}
    }
    private void collectUnreferenced()throws IOException {
        Set<String> live;
        synchronized(this){live=new HashSet<>(committedObjects);for(Entry e:entries.values())live.addAll(e.blocks.values());}
        try(DirectoryStream<Path> paths=Files.newDirectoryStream(objects)) {
            for(Path path:paths)if(isId(path.getFileName().toString()) && !live.contains(path.getFileName().toString()))Files.delete(path);
        }
        syncDirectory(objects);
        try(DirectoryStream<Path> paths=Files.newDirectoryStream(root,"state-*")) {
            for(Path path:paths)if(isId(path.getFileName().toString().substring(6)))Files.delete(path);
        }
        syncDirectory(root);
    }

    private static byte[] encode(TreeMap<String,Entry> view)throws IOException {
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();DataOutputStream out=new DataOutputStream(bytes);
        out.writeInt(1);out.writeInt(view.size());int count=0;
        for(Map.Entry<String,Entry> item:view.entrySet()) {
            Entry e=item.getValue();count+=e.blocks.size();
            if(count>MAX_CHUNKS)throw new IOException("Encrypted-store chunk limit");
            out.writeUTF(item.getKey());out.writeUTF(e.id);out.writeLong(e.size);out.writeInt(e.blocks.size());
            for(Map.Entry<Long,String> b:e.blocks.entrySet()){out.writeLong(b.getKey());out.writeUTF(b.getValue());}
        }
        out.flush();byte[] result=bytes.toByteArray();
        if(result.length>MAX_INDEX){wipe(result);throw new IOException("Encrypted-store index limit");}return result;
    }
    private void decode(byte[] bytes)throws IOException {
        DataInputStream in=new DataInputStream(new ByteArrayInputStream(bytes));
        if(in.readInt()!=1)throw new IOException("Unknown encrypted index format");
        int count=in.readInt(),total=0;if(count<0 || count>MAX_FILES)throw new IOException("Invalid encrypted index count");
        Set<String> ids=new HashSet<>(),objects=new HashSet<>();
        for(int i=0;i<count;i++) {
            String name=in.readUTF();name(name);Entry e=new Entry(in.readUTF());e.size=in.readLong();int chunks=in.readInt();
            if(!isId(e.id) || !ids.add(e.id) || e.size<0 || e.size>MAX_SIZE || chunks<0 || chunks>MAX_CHUNKS-total)
                throw new IOException("Invalid encrypted index entry");
            total+=chunks;
            for(int j=0;j<chunks;j++) {
                long index=in.readLong();String object=in.readUTF();
                if(index<0 || index>=(e.size+CHUNK_SIZE-1)/CHUNK_SIZE || !isId(object) || !objects.add(object) || e.blocks.put(index,object)!=null)
                    throw new IOException("Invalid encrypted chunk reference");
            }
            if(entries.put(name,e)!=null)throw new IOException("Duplicate encrypted file name");
        }
        if(in.available()!=0)throw new IOException("Trailing encrypted index bytes");
    }

    @Override public void close()throws IOException {
        if(commits.isHeldByCurrentThread())throw new IllegalStateException("Close during CE commit");
        commits.lock();try{synchronized(this){
        if(closed)return;closed=true;wipe(indexKey);wipe(contentKey);indexKey=null;contentKey=null;
        for(byte[] value:readCache.values())wipe(value);readCache.clear();
        for(Entry e:entries.values())for(byte[] value:e.dirty.values())wipe(value);entries.clear();
        try {try{ownerLock.release();}finally{owner.close();}}
        finally{synchronized(OPEN){OPEN.remove(root);}}
        }}finally{commits.unlock();}
    }
    private byte[] context(String purpose){return concat("Pocket CE "+purpose+" v1",storeId);}
    private static byte[] random(int count){byte[] value=new byte[count];RANDOM.nextBytes(value);return value;}
    private static String id(){byte[] bytes=random(16);char[] out=new char[32];String hex="0123456789abcdef";for(int i=0;i<bytes.length;i++){int b=bytes[i]&255;out[2*i]=hex.charAt(b>>>4);out[2*i+1]=hex.charAt(b&15);}return new String(out);}
    private static boolean isId(String id){return id!=null && id.matches("[0-9a-f]{32}");}
    private static byte[] utf8(String s){return s.getBytes(java.nio.charset.StandardCharsets.UTF_8);}
    private static byte[] concat(String prefix,byte[] suffix){return concat(utf8(prefix),suffix);}
    private static byte[] concat(byte[]... values) {
        int count=0;for(byte[] b:values)count+=b.length;byte[] all=new byte[count];int at=0;
        for(byte[] b:values){System.arraycopy(b,0,all,at,b.length);at+=b.length;}return all;
    }
    private static void wipe(byte[] value){if(value!=null)Arrays.fill(value,(byte)0);}
    // HKDF-SHA256 extract/expand (one 32-byte output block). The input must
    // already have high entropy; this is intentionally not a password KDF.
    static byte[] derive(byte[] key,byte[] salt,byte[] info)throws GeneralSecurityException {
        Mac mac=Mac.getInstance("HmacSHA256");mac.init(new SecretKeySpec(salt,"HmacSHA256"));byte[] prk=mac.doFinal(key);
        try {mac.init(new SecretKeySpec(prk,"HmacSHA256"));mac.update(info);mac.update((byte)1);return mac.doFinal();}
        finally{wipe(prk);}
    }
    private static byte[] seal(byte[] key,byte[] aad,byte[] clear)throws GeneralSecurityException {
        byte[] nonce=random(12);Cipher cipher=Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE,new SecretKeySpec(key,"AES"),new GCMParameterSpec(128,nonce));cipher.updateAAD(aad);
        return concat(nonce,cipher.doFinal(clear));
    }
    private static byte[] unseal(byte[] key,byte[] aad,byte[] sealed)throws GeneralSecurityException,IOException {
        if(sealed.length<28)throw new IOException("Truncated encrypted record");
        Cipher cipher=Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE,new SecretKeySpec(key,"AES"),new GCMParameterSpec(128,sealed,0,12));cipher.updateAAD(aad);
        return cipher.doFinal(sealed,12,sealed.length-12);
    }
    private static void writeNew(Path path,byte[] bytes)throws IOException {
        try(FileChannel file=FileChannel.open(path,EnumSet.of(StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE),
            PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rw-------")))) {
            ByteBuffer data=ByteBuffer.wrap(bytes);while(data.hasRemaining())file.write(data);file.force(true);
        }
    }
    private static byte[] readBounded(Path path,int max)throws IOException {
        long size=Files.size(path);if(size<0 || size>max)throw new IOException("Invalid encrypted record size");
        // Read through a bounded stream as well: a damaged concurrent writer
        // must not turn a small stat result into an unbounded allocation.
        try(InputStream in=Files.newInputStream(path)) {
            ByteArrayOutputStream out=new ByteArrayOutputStream();byte[] buffer=new byte[4096];int n,total=0;
            while((n=in.read(buffer))!=-1){total+=n;if(total>max)throw new IOException("Oversized encrypted record");out.write(buffer,0,n);}
            return out.toByteArray();
        }
    }
    private static void syncDirectory(Path path)throws IOException {
        try(FileChannel directory=FileChannel.open(path,StandardOpenOption.READ)){directory.force(true);}
    }
}
