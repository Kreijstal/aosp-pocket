// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.storage;

import java.io.*;
import java.security.GeneralSecurityException;
import java.util.*;

/** Encrypted POSIX namespace. Host directories contain only opaque inode IDs. */
public final class CeNamespace {
    public static final int REG=0100000,DIR=0040000,LNK=0120000;
    public static final String[] ROOTS={"/data/user/0","/data/system_ce/0","/data/misc_ce/0","/data/media/0"};
    private static final String INDEX="namespace-v1";
    public static final class FsException extends IOException {
        private static final long serialVersionUID=1;
        public final int errno;
        public FsException(int errno){super("CE filesystem error "+errno);this.errno=errno;}
    }
    public static final class Outside extends IOException {
        private static final long serialVersionUID=1;
        public final String path;
        Outside(String path){this.path=path;}
    }
    public static final class Node {
        public int id,parent,mode,uid,gid,links;
        public long atime,mtime,ctime;
        public String target="";
        public final TreeMap<String,Integer> children=new TreeMap<>();
        public final TreeMap<String,byte[]> xattrs=new TreeMap<>();
        public boolean directory(){return (mode&0170000)==DIR;}
        public boolean regular(){return (mode&0170000)==REG;}
        public boolean symlink(){return (mode&0170000)==LNK;}
    }
    public final EncryptedStore store;
    private final TreeMap<Integer,Node> nodes=new TreeMap<>();
    private int next=7;
    // Hashes describe the current in-memory index, including uncommitted
    // writes. Compare before calling store.write, which otherwise has to read
    // every old chunk after file data has evicted it from the shared cache.
    private byte[][] serializedChunks=new byte[0][];
    private boolean metadataDirty;
    private long metadataRevision;
    public boolean autoCommit=true;
    public CeNamespace(EncryptedStore store,boolean create)throws IOException,GeneralSecurityException {
        this.store=store;
        if(create) {
            for(int i=0;i<ROOTS.length;i++){Node n=newNode(i+3,0,DIR|0771,1000,1000);nodes.put(n.id,n);}
            store.createFile(INDEX);sync();
        } else {
            long size=store.size(INDEX);if(size>16*1024*1024)throw new IOException("Oversized CE namespace");
            byte[] raw=new byte[(int)size];store.read(INDEX,0,raw,0,raw.length);
            try {decode(raw);serializedChunks=chunkHashes(raw);}finally{Arrays.fill(raw,(byte)0);}
            boolean removed=false;
            for(Iterator<Node> it=nodes.values().iterator();it.hasNext();) {
                Node n=it.next();if(n.links==0){if(n.regular())store.delete(content(n.id));it.remove();removed=true;}
            }
            if(removed)sync();
        }
    }
    private static long now(){return System.currentTimeMillis();}
    // Runtime metadata changes are buffered like ordinary filesystem writes.
    // Explicit sync still commits the namespace and file contents atomically.
    public synchronized void markDirty(){metadataDirty=true;metadataRevision++;}
    public synchronized boolean hasPendingChanges(){return metadataDirty || store.hasPendingChanges();}
    private void commit()throws IOException,GeneralSecurityException {if(autoCommit)sync();}
    private static Node newNode(int id,int parent,int mode,int uid,int gid) {
        Node n=new Node();n.id=id;n.parent=parent;n.mode=mode;n.uid=uid;n.gid=gid;n.links=(mode&0170000)==DIR?2:1;
        n.atime=n.mtime=n.ctime=now();return n;
    }
    public synchronized Node node(int id)throws FsException {Node n=nodes.get(id);if(n==null)throw new FsException(2);return n;}
    public static int root(String path) {
        for(int i=0;i<ROOTS.length;i++)if(path.equals(ROOTS[i]) || path.startsWith(ROOTS[i]+"/"))return i+3;
        return 0;
    }
    public static String alias(String path) {
        for(String base:new String[]{"/storage/emulated","/mnt/runtime/default/emulated","/mnt/runtime/read/emulated","/mnt/runtime/write/emulated"})
            if(path.startsWith(base+"/"))return "/data/media"+path.substring(base.length());
        if(path.startsWith("/mnt/user/")) {
            int slash=path.indexOf('/',10);
            if(slash>10 && (path.substring(slash).equals("/primary") || path.substring(slash).startsWith("/primary/")))
                return "/data/media/"+path.substring(10,slash)+path.substring(slash+8);
        }
        String[] aliases={"/data/data","/sdcard","/storage/emulated/0","/storage/self/primary","/mnt/user/0/primary",
            "/mnt/runtime/default/emulated/0","/mnt/runtime/read/emulated/0","/mnt/runtime/write/emulated/0"};
        for(int i=0;i<aliases.length;i++)if(path.equals(aliases[i]) || path.startsWith(aliases[i]+"/"))
            return (i==0?ROOTS[0]:ROOTS[3])+path.substring(aliases[i].length());
        return path;
    }
    public synchronized String path(int id)throws IOException {
        Node n=node(id);if(n.id>=3 && n.id<=6)return ROOTS[n.id-3];
        if(!n.directory())throw new FsException(20);
        Node parent=node(n.parent);String name=null;
        for(Map.Entry<String,Integer> item:parent.children.entrySet())if(item.getValue()==id){name=item.getKey();break;}
        if(name==null)throw new FsException(2);return path(parent.id)+"/"+name;
    }
    public static void access(Node n,int uid,int gid,int[] groups,int bits)throws FsException {
        if(uid==0)return;int shift=n.uid==uid?6:n.gid==gid?3:0;
        if(shift==0)for(int group:groups)if(group==n.gid){shift=3;break;}
        if(((n.mode>>shift)&bits)!=bits)throw new FsException(13);
    }
    public synchronized Node resolve(String path,int uid,int gid,int[] groups,boolean follow)throws IOException {
        return resolve(path,uid,gid,groups,follow,0);
    }
    private Node resolve(String path,int uid,int gid,int[] groups,boolean follow,int depth)throws IOException {
        if(depth>40)throw new FsException(40);path=alias(path);int root=root(path);if(root==0)throw new Outside(path);
        Node current=node(root);String tail=path.substring(ROOTS[root-3].length());String[] parts=tail.split("/",-1);
        for(int i=0;i<parts.length;i++) {
            String part=parts[i];if(part.isEmpty())continue;
            if(!current.directory())throw new FsException(20);access(current,uid,gid,groups,1);
            if(part.equals("."))continue;
            if(part.equals("..")) {
                if(current.parent==0) {
                    String base=ROOTS[current.id-3];base=base.substring(0,base.lastIndexOf('/'));
                    StringBuilder rest=new StringBuilder(base);for(int j=i+1;j<parts.length;j++)rest.append('/').append(parts[j]);
                    throw new Outside(rest.toString());
                }
                current=node(current.parent);continue;
            }
            Integer id=current.children.get(part);if(id==null)throw new FsException(2);Node next=node(id);
            boolean last=true;for(int j=i+1;j<parts.length;j++)if(!parts[j].isEmpty()){last=false;break;}
            if(next.symlink() && (follow || !last || path.endsWith("/"))) {
                String target=next.target.startsWith("/")?next.target:path(current.id)+"/"+next.target;
                StringBuilder rest=new StringBuilder(target);for(int j=i+1;j<parts.length;j++)rest.append('/').append(parts[j]);
                return resolve(rest.toString(),uid,gid,groups,follow,depth+1);
            }
            current=next;
        }
        if(path.endsWith("/") && !current.directory())throw new FsException(20);return current;
    }
    private Node parent(String path,int uid,int gid,int[] groups)throws IOException {
        int slash=path.lastIndexOf('/');if(slash<0)throw new FsException(22);
        Node parent=resolve(path.substring(0,slash),uid,gid,groups,true);if(!parent.directory())throw new FsException(20);
        access(parent,uid,gid,groups,3);return parent;
    }
    private static String basename(String path)throws FsException {
        String name=path.substring(path.lastIndexOf('/')+1);
        if(name.isEmpty() || name.equals(".") || name.equals(".."))throw new FsException(22);
        if(name.getBytes(java.nio.charset.StandardCharsets.UTF_8).length>255)throw new FsException(36);return name;
    }
    public Node create(String path,int mode,int uid,int gid,int[] groups,String target)throws IOException,GeneralSecurityException {
        Node n;synchronized(this) {
        path=alias(path);Node parent=parent(path,uid,gid,groups);String name=basename(path);
        if(parent.children.containsKey(name))throw new FsException(17);
        if(next==Integer.MAX_VALUE || nodes.size()>=9999)throw new FsException(28);
        n=newNode(next++,parent.id,mode,uid,(parent.mode&02000)!=0?parent.gid:gid);
        if((parent.mode&02000)!=0 && n.directory())n.mode|=02000;
        n.target=target==null?"":target;if(n.regular())store.createFile(content(n.id));
        nodes.put(n.id,n);parent.children.put(name,n.id);if(n.directory())parent.links++;parent.mtime=parent.ctime=now();markDirty();}commit();return n;
    }
    public Node remove(String path,boolean directory,int uid,int gid,int[] groups)throws IOException,GeneralSecurityException {
        Node n;synchronized(this) {
        path=alias(path);Node p=parent(path,uid,gid,groups);String name=basename(path);Integer id=p.children.get(name);if(id==null)throw new FsException(2);
        n=node(id);if(n.directory()!=directory)throw new FsException(n.directory()?21:20);
        sticky(p,n,uid);if(n.directory() && !n.children.isEmpty())throw new FsException(39);
        p.children.remove(name);if(n.directory()){p.links--;n.links=0;}else n.links--;
        p.mtime=p.ctime=n.ctime=now();markDirty();}commit();
        // Keep unlinked inodes until the session ends. FUSE mappings may outlive
        // the final open fd; reusing or deleting their data here is unsafe.
        return n;
    }
    public boolean reclaimFile(int id)throws IOException,GeneralSecurityException {
        synchronized(this){Node n=nodes.get(id);if(n==null || !n.regular() || n.links!=0)return false;
        store.delete(content(id));nodes.remove(id);markDirty();}commit();return true;
    }
    private static void sticky(Node p,Node n,int uid)throws FsException {
        if((p.mode&01000)!=0 && uid!=0 && uid!=p.uid && uid!=n.uid)throw new FsException(1);
    }
    public void link(String from,String to,int uid,int gid,int[] groups)throws IOException,GeneralSecurityException {
        synchronized(this){Node n=resolve(from,uid,gid,groups,false);if(n.directory())throw new FsException(1);
        Node p=parent(alias(to),uid,gid,groups);String name=basename(to);if(p.children.containsKey(name))throw new FsException(17);
        p.children.put(name,n.id);n.links++;n.ctime=p.ctime=p.mtime=now();markDirty();}commit();
    }
    public void rename(String from,String to,boolean noReplace,int uid,int gid,int[] groups)throws IOException,GeneralSecurityException {
        synchronized(this){from=alias(from);to=alias(to);Node a=parent(from,uid,gid,groups),b=parent(to,uid,gid,groups);
        String oldName=basename(from),newName=basename(to);Integer id=a.children.get(oldName);if(id==null)throw new FsException(2);
        Node n=node(id);sticky(a,n,uid);Integer target=b.children.get(newName);if(target!=null && target.equals(id))return;
        if(target!=null) {
            if(noReplace)throw new FsException(17);Node other=node(target);sticky(b,other,uid);
            if(other.directory()!=n.directory())throw new FsException(other.directory()?21:20);
            if(other.directory() && !other.children.isEmpty())throw new FsException(39);
        }
        if(n.directory())for(Node p=b;p.parent!=0;p=node(p.parent))if(p.id==n.id)throw new FsException(22);
        if(target!=null) {Node other=node(target);if(other.directory()){b.links--;other.links=0;}else other.links--;other.ctime=now();}
        a.children.remove(oldName);b.children.put(newName,n.id);
        if(n.directory()){a.links--;b.links++;n.parent=b.id;}
        a.mtime=a.ctime=b.mtime=b.ctime=n.ctime=now();markDirty();}commit();
    }
    public static String content(int id){return "inode-"+id;}
    public synchronized long size(Node n)throws IOException {return n.regular()?store.size(content(n.id)):n.symlink()?n.target.getBytes(java.nio.charset.StandardCharsets.UTF_8).length:0;}
    private static byte[][] chunkHashes(byte[] raw)throws GeneralSecurityException {
        int chunk=EncryptedStore.CHUNK_SIZE;byte[][] hashes=new byte[(raw.length+chunk-1)/chunk][];
        java.security.MessageDigest digest=java.security.MessageDigest.getInstance("SHA-256");
        for(int i=0;i<hashes.length;i++){int at=i*chunk;digest.update(raw,at,Math.min(chunk,raw.length-at));hashes[i]=digest.digest();}
        return hashes;
    }
    public void sync()throws IOException,GeneralSecurityException {
        // The commit lease serializes durable snapshots. Filesystem monitors
        // cover capture only; encryption, fsync and collection run outside them.
        try(EncryptedStore.Commit commit=store.beginCommit()) {
            long revision;
            synchronized(this) {
                ByteArrayOutputStream bytes=new ByteArrayOutputStream();DataOutputStream out=new DataOutputStream(bytes);
                out.writeInt(2);out.writeInt(next);out.writeInt(nodes.size());
                for(Node n:nodes.values()) {
                    out.writeInt(n.id);out.writeInt(n.parent);out.writeInt(n.mode);out.writeInt(n.uid);out.writeInt(n.gid);out.writeInt(n.links);
                    out.writeLong(n.atime);out.writeLong(n.mtime);out.writeLong(n.ctime);out.writeUTF(n.target);out.writeInt(n.children.size());
                    for(Map.Entry<String,Integer> e:n.children.entrySet()){out.writeUTF(e.getKey());out.writeInt(e.getValue());}
                    out.writeInt(n.xattrs.size());for(Map.Entry<String,byte[]> x:n.xattrs.entrySet()){out.writeUTF(x.getKey());out.writeInt(x.getValue().length);out.write(x.getValue());}
                }
                out.flush();byte[] raw=bytes.toByteArray();
                try {
                    byte[][] next=chunkHashes(raw),previous=serializedChunks;
                    // A partially failed resize/write invalidates comparisons
                    // until a complete index image has been prepared again.
                    serializedChunks=null;store.resize(INDEX,raw.length);
                    for(int i=0;i<next.length;i++)if(previous==null || i>=previous.length || !Arrays.equals(next[i],previous[i])) {
                        int at=i*EncryptedStore.CHUNK_SIZE;
                        store.write(INDEX,at,raw,at,Math.min(EncryptedStore.CHUNK_SIZE,raw.length-at));
                    }
                    serializedChunks=next;
                    commit.capture();revision=metadataRevision;
                }finally{Arrays.fill(raw,(byte)0);}
            }
            commit.flush();
            synchronized(this){if(metadataRevision==revision)metadataDirty=false;}
        }
    }
    private void decode(byte[] raw)throws IOException {
        DataInputStream in=new DataInputStream(new ByteArrayInputStream(raw));int version=in.readInt();if(version!=1 && version!=2)throw new IOException("Unknown CE namespace version");
        next=in.readInt();int count=in.readInt();if(count<4 || count>10000 || next<7)throw new IOException("Invalid CE namespace count");
        for(int i=0;i<count;i++) {
            Node n=new Node();n.id=in.readInt();n.parent=in.readInt();n.mode=in.readInt();n.uid=in.readInt();n.gid=in.readInt();n.links=in.readInt();
            n.atime=in.readLong();n.mtime=in.readLong();n.ctime=in.readLong();n.target=in.readUTF();int children=in.readInt();
            if(n.id<3 || n.id>=next || children<0 || children>10000 || (!n.directory() && children!=0))throw new IOException("Invalid CE inode");
            for(int j=0;j<children;j++){String name=in.readUTF();if(!basename(name).equals(name) || n.children.put(name,in.readInt())!=null)throw new IOException("Invalid CE name");}
            if(version>=2) {
                int attrs=in.readInt();if(attrs<0 || attrs>64)throw new IOException("Invalid CE attribute count");
                for(int j=0;j<attrs;j++){String name=in.readUTF();int size=in.readInt();if(size<0 || size>16384 || !name.startsWith("user."))throw new IOException("Invalid CE attribute");byte[] value=new byte[size];in.readFully(value);if(n.xattrs.put(name,value)!=null)throw new IOException("Duplicate CE attribute");}
            }
            if(nodes.put(n.id,n)!=null)throw new IOException("Duplicate CE inode");
        }
        if(in.available()!=0)throw new IOException("Trailing CE namespace data");
        for(int i=3;i<=6;i++)if(!node(i).directory() || node(i).parent!=0)throw new IOException("Invalid CE root");
        for(Node n:nodes.values())for(int id:n.children.values())node(id);
    }
}
