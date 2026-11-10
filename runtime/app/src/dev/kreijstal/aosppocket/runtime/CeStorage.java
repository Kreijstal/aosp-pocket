// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.net.*;
import android.os.*;
import android.system.*;
import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.security.*;
import java.util.*;
import java.util.concurrent.*;
import dev.kreijstal.aosppocket.storage.*;
import dev.kreijstal.aosppocket.storage.CeNamespace.Node;
import dev.kreijstal.aosppocket.storage.CeNamespace.FsException;

/** Owns the guest CE keys, encrypted namespace, AppFuse inode transport and RPC. */
final class CeStorage implements AutoCloseable {
    static final int MAGIC=0x31454350;
    // Linux arm64 UAPI values hidden from the public SDK stubs.
    private static final int O_DIRECTORY=0x4000,O_PATH=0x200000,AT_SYMLINK_NOFOLLOW=0x100,AT_REMOVEDIR=0x200;
    final File root;
    private final String[] roots;
    private final LocalSocket bound=new LocalSocket();
    private final LocalServerSocket server;
    private final ThreadPoolExecutor workers=new ThreadPoolExecutor(8,8,0,TimeUnit.SECONDS,new ArrayBlockingQueue<>(128));
    private final ScheduledExecutorService checkpoints=Executors.newSingleThreadScheduledExecutor(r->new Thread(r,"PocketCeCheckpoint"));
    private final Object rootAttrs=new Object();
    private final Set<LocalSocket> peers=ConcurrentHashMap.newKeySet();
    private volatile boolean closed;
    private volatile CeNamespace namespace;
    private volatile CeFuse fuse;
    private final Runnable transportDead;
    private volatile boolean deadTransport;
    private byte[] diskSecret,preparedSecret;
    private volatile boolean captureArmed;
    private final Map<Integer,Integer> opened=new HashMap<>();
    private final Set<Integer> touched=new HashSet<>();
    private volatile boolean draining;
    static boolean enabled(File root){return new File(root,".pocket-ce-enabled").isFile();}
    CeStorage(File root)throws Exception{this(root,()->{});}
    CeStorage(File root,Runnable transportDead)throws Exception {
        this.transportDead=transportDead;
        this.root=root;roots=CeMigration.roots(root);
        CeMigration.sealPlaceholders(root);
        File cache=new File(root,".pocket-ce-dirs");
        File[] cached=cache.listFiles();
        if(cached!=null)for(File dir:cached)sealDirectory(dir);
        Path endpoint=new File(root,"dev/socket/pocket_ce").toPath();Files.deleteIfExists(endpoint);
        bound.bind(new LocalSocketAddress(endpoint.toString(),LocalSocketAddress.Namespace.FILESYSTEM));
        Os.chmod(endpoint.toString(),0600);server=new LocalServerSocket(bound.getFileDescriptor());
        fuse=new CeFuse(this);publishDevice(fuse.device);
        new Thread(this::accept,"PocketCeRpc").start();
        checkpoints.scheduleWithFixedDelay(this::checkpoint,250,250,TimeUnit.MILLISECONDS);
    }
    private void publishDevice(long id)throws IOException {
        Path device=new File(root,".pocket-ce-device").toPath(),next=new File(root,".pocket-ce-device.new").toPath();
        Files.write(next,le(8).putLong(id).array());Files.move(next,device,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
    }
    private ParcelFileDescriptor openContent(int id,int mode)throws Exception {
        CeFuse current=fuse;if(current==null)throw new FsException(5);
        try{return current.open(id,mode);}
        catch(IOException failure){transportFailed(current);throw failure;}
    }
    // The host AppFuse mount can stop serving under a live guest: after an
    // app update killed the previous process, every later open was refused
    // until the guest restarted. A new mount cannot be swapped in, because
    // every guest process (zygote's children included) caches the mount's
    // device id in the loader; fcntl locks on new descriptors then break, and
    // a framework-only restart leaves netd, input and audio state stale. So
    // ask for a full guest restart, once. Only a mount proven dead by its
    // callback-free root inode counts; one file's error never restarts.
    private void transportFailed(CeFuse failed) {
        if(closed || draining || deadTransport || fuse!=failed || failed.alive())return;
        synchronized(this){if(deadTransport)return;deadTransport=true;}
        android.util.Log.e("PocketCE","Encrypted file transport stopped; restarting the guest");
        transportDead.run();
    }
    private void checkpoint() {
        CeNamespace ns=namespace;
        if(ns==null || closed || draining)return;
        try {
            if(namespace==ns && !closed && !draining && ns.hasPendingChanges())ns.sync();
        }catch(Exception error){android.util.Log.e("PocketCE","Encrypted storage checkpoint failed; pending writes retained",error);}
    }
    private void accept() {
        while(!closed)try {
            LocalSocket peer=server.accept();peers.add(peer);
            try{workers.execute(()->serve(peer));}catch(RejectedExecutionException busy){peers.remove(peer);peer.close();}
        }catch(IOException error){if(!closed)android.util.Log.e("PocketCE","Encrypted storage endpoint failed",error);}
    }
    private static ByteBuffer le(int size){return ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN);}
    private static String string(DataInputStream in,int count)throws IOException {
        if(count<0 || count>4096)throw new IOException("CE path bound");byte[] bytes=new byte[count];in.readFully(bytes);
        String s=new String(bytes,java.nio.charset.StandardCharsets.UTF_8);if(s.indexOf('\0')>=0)throw new IOException("CE path NUL");return s;
    }
    private static final class Request {
        int op,nr,uid,gid;long[] a=new long[6];int[] groups;String p,q;byte[] data;
    }
    private static final class Reply implements AutoCloseable {
        long value;byte[] data=new byte[0];final List<ParcelFileDescriptor> fds=new ArrayList<>();
        Reply(long value){this.value=value;}
        @Override public void close(){for(ParcelFileDescriptor fd:fds)try{fd.close();}catch(IOException ignored){}}
    }
    private void serve(LocalSocket peer) {
        try(LocalSocket client=peer) {
            if(client.getPeerCredentials().getUid()!=android.os.Process.myUid())throw new IOException("CE owner mismatch");
            client.setSoTimeout(30000);DataInputStream in=new DataInputStream(client.getInputStream());byte[] header=new byte[88];in.readFully(header);
            ByteBuffer b=ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);if(b.getInt()!=MAGIC || b.getInt()!=1)throw new IOException("CE protocol");
            Request r=new Request();r.op=b.getInt();r.nr=b.getInt();r.uid=b.getInt();r.gid=b.getInt();
            for(int i=0;i<6;i++)r.a[i]=b.getLong();int p=b.getInt(),q=b.getInt(),length=b.getInt(),groups=b.getInt();
            if(length<0 || length>16384 || groups<0 || groups>64)throw new IOException("CE request bound");
            r.p=string(in,p);r.q=string(in,q);r.data=new byte[length];in.readFully(r.data);r.groups=new int[groups];
            for(int i=0;i<groups;i++)r.groups[i]=Integer.reverseBytes(in.readInt());
            Reply reply;
            try{reply=handle(r);}
            catch(CeNamespace.Outside outside){reply=new Reply(-18);reply.data=outside.path.getBytes("UTF-8");}
            catch(FsException denied){reply=new Reply(-denied.errno);}
            catch(ErrnoException error){reply=new Reply(-error.errno);}
            catch(GeneralSecurityException error){reply=new Reply(-5);}
            catch(Exception error){android.util.Log.e("PocketCE","CE operation failed: "+r.op+"/"+r.nr+" "+error.getClass().getSimpleName());reply=new Reply(-5);}
            finally{Arrays.fill(r.data,(byte)0);}
            try(Reply result=reply) {
                FileDescriptor[] fds=new FileDescriptor[result.fds.size()];for(int i=0;i<fds.length;i++)fds[i]=result.fds.get(i).getFileDescriptor();
                client.setFileDescriptorsForSend(fds);client.getOutputStream().write(0xa5);client.setFileDescriptorsForSend(null);
                client.getOutputStream().write(le(16).putLong(result.value).putInt(result.data.length).putInt(fds.length).array());
                client.getOutputStream().write(result.data);
            }
        }catch(IOException ignored){}finally{peers.remove(peer);}
    }
    synchronized void unlock(byte[] secret)throws Exception {
        if(!enabled(root))throw new FsException(95);
        byte[] supplied=secret.length==0?CeDefaultKey.load(root):secret;
        try {
            if(namespace!=null){if(!MessageDigest.isEqual(diskSecret,supplied))throw new FsException(13);return;}
            EncryptedStore store=EncryptedStore.open(new File(root,".pocket-ce-v1").toPath(),supplied);
            try{
                CeNamespace ns=new CeNamespace(store,false);
                if(!Arrays.equals(ns.roots(),roots))throw new IOException("CE vault layout does not match the guest profile");
                ns.autoCommit=false;diskSecret=supplied.clone();namespace=ns;
            }
            catch(Exception error){store.close();throw error;}
            synchronized(rootAttrs){synchronized(namespace){for(int id=3;id<roots.length+3;id++)mirrorRoot(namespace.node(id));}}
        }finally{if(supplied!=secret)Arrays.fill(supplied,(byte)0);}
    }
    synchronized void prepareAuth(byte[] secret)throws Exception {
        if(secret.length==0){if(namespace==null || diskSecret==null)throw new FsException(13);CeDefaultKey.prepare(root,diskSecret);}
        else {unlock(secret);CeDefaultKey.prepareSecure(root);}
    }
    private CeNamespace unlocked()throws FsException {CeNamespace ns=namespace;if(ns==null)throw new FsException(13);return ns;}
    // As with file-based encryption, a CE root's own user.* attributes stay
    // readable while locked. UserDataPreparer reads user.serial at boot and
    // destroys the user's DE and CE storage on any error but ENODATA.
    private File rootXattrs(int id){return new File(root,".pocket-ce-root-xattrs/"+id);}
    private void mirrorRoot(Node n)throws IOException {
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();DataOutputStream out=new DataOutputStream(bytes);
        out.writeInt(n.xattrs.size());
        for(Map.Entry<String,byte[]> e:n.xattrs.entrySet()){out.writeUTF(e.getKey());out.writeInt(e.getValue().length);out.write(e.getValue());}
        File file=rootXattrs(n.id),next=new File(file.getPath()+".new");file.getParentFile().mkdirs();
        try(FileOutputStream stream=new FileOutputStream(next)){stream.write(bytes.toByteArray());stream.getFD().sync();}
        Files.move(next.toPath(),file.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
    }
    private byte[] lockedRootXattr(int id,String name)throws IOException {
        File file=rootXattrs(id);if(!file.isFile())return null;
        try(DataInputStream in=new DataInputStream(new FileInputStream(file))) {
            for(int count=in.readInt();count>0;count--) {
                String key=in.readUTF();byte[] value=new byte[in.readInt()];in.readFully(value);
                if(key.equals(name))return value;
            }
        }
        return null;
    }
    private String path(String value)throws IOException {
        String directory=root.getPath()+"/.pocket-ce-dirs/";
        if(value.startsWith(directory)) {
            String tail=value.substring(directory.length());int slash=tail.indexOf('/');String id=slash<0?tail:tail.substring(0,slash);
            try{return unlocked().path(Integer.parseInt(id))+(slash<0?"":tail.substring(slash));}
            catch(NumberFormatException invalid){throw new FsException(2);}
        }
        if(value.startsWith(root.getPath()+"/system-data/"))value="/data/"+value.substring(root.getPath().length()+13);
        else if(value.startsWith(root.getPath()+"/"))value=value.substring(root.getPath().length());
        return CeNamespace.alias(value);
    }
    private int rootId(String path) {
        for(int i=0;i<roots.length;i++)if(path.equals(roots[i]) || path.startsWith(roots[i]+"/"))return i+3;
        return 0;
    }
    private Node fdNode(Request r)throws IOException {
        CeNamespace ns=unlocked();String directory=root.getPath()+"/.pocket-ce-dirs/";
        if(r.p.startsWith(directory))try{return ns.node(Integer.parseInt(r.p.substring(directory.length())));}catch(NumberFormatException bad){throw new FsException(9);}
        if(r.a[0]!=fuse.device || r.a[1]<3 || r.a[1]>Integer.MAX_VALUE)throw new FsException(9);
        return ns.node((int)r.a[1]);
    }
    private static void sealDirectory(File dir)throws Exception {
        if(Files.isSymbolicLink(dir.toPath()) || !dir.isDirectory())throw new IOException("Invalid CE directory cache");
        Os.chmod(dir.getPath(),0500);
        String[] children=dir.list();
        if(children==null || children.length!=0)throw new IOException("Unexpected plaintext in CE directory cache");
    }
    private ParcelFileDescriptor directory(Node n)throws Exception {
        File dir=new File(root,".pocket-ce-dirs/"+n.id);if(!dir.isDirectory() && !dir.mkdirs())throw new IOException("CE directory cache");
        sealDirectory(dir);
        FileDescriptor fd=Os.open(dir.getPath(),OsConstants.O_RDONLY|O_DIRECTORY|OsConstants.O_CLOEXEC,0);
        try{return ParcelFileDescriptor.dup(fd);}finally{Os.close(fd);}
    }
    private ParcelFileDescriptor lockFile(Node n)throws IOException {
        File file=new File(root,".pocket-ce-locks/"+n.id);file.getParentFile().mkdirs();
        return ParcelFileDescriptor.open(file,ParcelFileDescriptor.MODE_CREATE|ParcelFileDescriptor.MODE_READ_WRITE);
    }
    private byte[] stat(Node n)throws IOException {
        ByteBuffer s=le(128);s.putLong(0,0xce01);s.putLong(8,n.id);s.putInt(16,n.mode);s.putInt(20,n.links);
        s.putInt(24,n.uid);s.putInt(28,n.gid);long size=n.regular()?unlocked().size(n):n.symlink()?n.target.getBytes("UTF-8").length:0;s.putLong(48,size);s.putInt(56,4096);s.putLong(64,(size+511)/512);
        long[] times={n.atime,n.mtime,n.ctime};for(int i=0;i<3;i++){s.putLong(72+i*16,times[i]/1000);s.putLong(80+i*16,(times[i]%1000)*1000000);}
        return s.array();
    }
    private Reply open(Node n,int flags,int uid,int gid,int[] groups)throws Exception {
        int mode=flags&3;CeNamespace.access(n,uid,gid,groups,mode==0?4:mode==1?2:6);
        if((flags&O_DIRECTORY)!=0 && !n.directory())throw new FsException(20);
        Reply reply=new Reply(0);
        try {
          if(n.directory()) {if(mode!=0 && (flags&O_PATH)==0)throw new FsException(21);reply.fds.add(directory(n));}
          else {
            if(!n.regular())throw new FsException(40);
            if((flags&OsConstants.O_TRUNC)!=0) {if(mode==0)throw new FsException(13);CeNamespace ns=unlocked();synchronized(ns){ns.store.resize(CeNamespace.content(n.id),0);n.mtime=n.ctime=System.currentTimeMillis();ns.markDirty();}}
            ParcelFileDescriptor data=openContent(n.id,mode==0?ParcelFileDescriptor.MODE_READ_ONLY:mode==1?ParcelFileDescriptor.MODE_WRITE_ONLY:ParcelFileDescriptor.MODE_READ_WRITE);
            reply.fds.add(data);reply.fds.add(lockFile(n));
          }
          reply.data=stat(n);return reply;
        }catch(Exception error){reply.close();throw error;}
    }
    private Reply handle(Request r)throws Exception {
        if(r.op==3) {
            if(r.uid!=0 && r.uid!=1000)throw new FsException(1);
            if(r.nr==0)return new Reply(namespace!=null?2:enabled(root)?1:0);
            if(r.nr==1){unlock(r.data);return new Reply(0);}
            if(r.nr==3){prepareAuth(r.data);return new Reply(0);}
            if(r.nr==9){CeDefaultKey.fixate(root);return new Reply(0);}
            // Test hook: stop the live AppFuse server, reproducing a dead
            // transport so the next open triggers the guest restart.
            if(r.nr==11){CeFuse current=fuse;if(current==null)throw new FsException(5);current.close();return new Reply(0);}
            if(r.nr==10)return new Reply(captureArmed?1:0);
            if(r.nr==4) {
                if(enabled(root) || !captureArmed || r.data.length<32)throw new FsException(22);
                synchronized(this){if(preparedSecret!=null)Arrays.fill(preparedSecret,(byte)0);preparedSecret=r.data.clone();}
                return new Reply(0);
            }
            if(r.nr==5){if(enabled(root))throw new FsException(22);synchronized(this){captureArmed=true;}return new Reply(0);}
            if(r.nr==6){synchronized(this){return new Reply(preparedSecret==null?0:preparedSecret.length);}}
            if(r.nr==7){synchronized(this){captureArmed=false;}return new Reply(0);}
            if(r.nr==8){synchronized(this){if(preparedSecret!=null)Arrays.fill(preparedSecret,(byte)0);preparedSecret=null;}return new Reply(0);}
            if(r.nr==2) {
                synchronized(this){if(!opened.isEmpty())throw new FsException(16);CeNamespace ns=namespace;if(ns!=null){ns.sync();ns.store.close();namespace=null;Arrays.fill(diskSecret,(byte)0);diskSecret=null;}}
                return new Reply(0);
            }
            throw new FsException(95);
        }
        boolean descriptor=r.op==2;Node n=null;String p=descriptor?"":path(r.p),q=r.q.isEmpty()?"":path(r.q);
        if(!descriptor && rootId(p)==0)for(String base:new String[]{"/data/user/","/data/system_ce/","/data/misc_ce/","/data/vendor_ce/","/data/media/","/storage/emulated/"})
            if(p.startsWith(base) && p.length()>base.length() && Character.isDigit(p.charAt(base.length())))throw new FsException(95);
        if(namespace==null && !descriptor && r.nr==79) {
            int id=rootId(p);if(id>=3 && id<roots.length+3 && p.equals(roots[id-3])) {
                n=new Node();n.id=id;n.mode=CeNamespace.DIR|0771;n.uid=n.gid=1000;n.links=2;
                if(roots.length==5) {
                    if(id==4)n.mode=CeNamespace.DIR|0770;
                    if(id==5){n.mode=CeNamespace.DIR|01771;n.gid=9998;}
                    if(id==6){n.mode=CeNamespace.DIR|02770;n.uid=n.gid=1023;}
                    if(id==7)n.uid=n.gid=0;
                }
                Reply reply=new Reply(0);reply.data=stat(n);return reply;
            }
        }
        if(namespace==null && !descriptor && (r.nr==8 || r.nr==9)) { // getxattr, lgetxattr
            int id=rootId(p);if(id>=3 && id<roots.length+3 && p.equals(roots[id-3])) {
                byte[] value=lockedRootXattr(id,r.q);if(value==null)throw new FsException(61);
                Reply reply=new Reply(value.length);reply.data=value;return reply;
            }
        }
        CeNamespace ns=unlocked();
        if(descriptor)n=fdNode(r);
        // open may enter AppFuse, whose callbacks need the namespace monitor.
        // Metadata operations use the monitor for snapshot consistency;
        // durable flushes run outside it, as does AppFuse open.
        if(r.nr==56)return openRequest(r,ns,p);
        if(r.nr==35){Node removed=ns.remove(p,(r.a[2]&AT_REMOVEDIR)!=0,r.uid,r.gid,r.groups);reclaimClosed(ns,removed.id);return new Reply(0);}
        if(r.nr==82 || r.nr==83){ns.sync();return new Reply(0);}
        if(r.nr>=5 && r.nr<=16)return xattrs(r,ns,descriptor,p,n);
        synchronized(ns){switch(r.nr) {
        case 79:case 80: { // statat, fstat
            if(!descriptor)n=ns.resolve(p,r.uid,r.gid,r.groups,(r.a[3]&AT_SYMLINK_NOFOLLOW)==0);
            Reply reply=new Reply(0);reply.data=stat(n);return reply;
        }
        case 48: { // faccessat
            n=ns.resolve(p,r.uid,r.gid,r.groups,true);CeNamespace.access(n,r.uid,r.gid,r.groups,(int)r.a[2]);return new Reply(0);
        }
        case 34:ns.create(p,CeNamespace.DIR|((int)r.a[2]&07777),r.uid,r.gid,r.groups,null);return new Reply(0);
        case 36:ns.create(p,CeNamespace.LNK|0777,r.uid,r.gid,r.groups,new String(r.data,"UTF-8"));return new Reply(0);
        case 37:ns.link(p,q,r.uid,r.gid,r.groups);return new Reply(0);
        case 38:case 276:if(r.nr==276 && (r.a[4]&~1L)!=0)throw new FsException(95);ns.rename(p,q,r.nr==276&&(r.a[4]&1)!=0,r.uid,r.gid,r.groups);return new Reply(0);
        case 78: {
            Reply reply=new Reply(0);
            if(descriptor)reply.data=ns.path(n.id).getBytes("UTF-8");
            else {n=ns.resolve(p,r.uid,r.gid,r.groups,false);if(!n.symlink())throw new FsException(22);reply.data=n.target.getBytes("UTF-8");}
            reply.value=reply.data.length;return reply;
        }
        case 49: {n=ns.resolve(p,r.uid,r.gid,r.groups,true);if(!n.directory())throw new FsException(20);CeNamespace.access(n,r.uid,r.gid,r.groups,1);Reply cwd=new Reply(0);cwd.fds.add(directory(n));return cwd;}
        case 17: {if(!n.directory())throw new FsException(20);Reply cwd=new Reply(0);cwd.data=(ns.path(n.id)+"\0").getBytes("UTF-8");return cwd;}
        case 46:case 45: {
            long size=descriptor?r.a[2]:r.a[1];if(!descriptor){n=ns.resolve(p,r.uid,r.gid,r.groups,true);CeNamespace.access(n,r.uid,r.gid,r.groups,2);}
            if(!n.regular())throw new FsException(21);ns.store.resize(CeNamespace.content(n.id),size);n.mtime=n.ctime=System.currentTimeMillis();ns.markDirty();return new Reply(0);
        }
        case 52:case 53:case 55:case 54: {
            if(!descriptor)n=ns.resolve(p,r.uid,r.gid,r.groups,r.nr!=54 || (r.a[4]&AT_SYMLINK_NOFOLLOW)==0);
            if(r.uid!=0 && r.uid!=n.uid)throw new FsException(1);
            if(r.nr==52 || r.nr==53)n.mode=(n.mode&0170000)|((int)(descriptor?r.a[2]:r.a[2])&07777);
            else {int uid=(int)(descriptor?r.a[2]:r.a[2]),gid=(int)(descriptor?r.a[3]:r.a[3]);if(r.uid!=0 && ((uid!=-1&&uid!=n.uid)||(gid!=-1&&gid!=r.gid)))throw new FsException(1);if(uid!=-1)n.uid=uid;if(gid!=-1)n.gid=gid;}
            n.ctime=System.currentTimeMillis();ns.markDirty();return new Reply(0);
        }
        case 88: {n=ns.resolve(p,r.uid,r.gid,r.groups,(r.a[3]&AT_SYMLINK_NOFOLLOW)==0);if(r.uid!=0 && r.uid!=n.uid)throw new FsException(1);
            long now=System.currentTimeMillis();if(r.data.length==0)n.atime=n.mtime=now;
            else {ByteBuffer t=ByteBuffer.wrap(r.data).order(ByteOrder.LITTLE_ENDIAN);long as=t.getLong(),an=t.getLong(),ms=t.getLong(),mn=t.getLong();
                if(an!=1073741822L)n.atime=an==1073741823L?now:as*1000+an/1000000;
                if(mn!=1073741822L)n.mtime=mn==1073741823L?now:ms*1000+mn/1000000;}
            n.ctime=now;ns.markDirty();return new Reply(0);}
        case 61: { // getdents64, offset supplied in a2
            if(!n.directory())throw new FsException(20);CeNamespace.access(n,r.uid,r.gid,r.groups,4);
            List<String> names=new ArrayList<>();names.add(".");names.add("..");names.addAll(n.children.keySet());
            int offset=(int)r.a[2],max=(int)Math.min(r.a[3],16384);if(offset<0)throw new FsException(22);ByteBuffer out=le(Math.max(0,max));
            for(int i=offset;i<names.size();i++) {
                String name=names.get(i);byte[] raw=name.getBytes("UTF-8");int length=(19+raw.length+1+7)&~7;
                if(out.remaining()<length){if(out.position()==0)throw new FsException(22);break;}
                int id=i==0?n.id:i==1?(n.parent==0?n.id:n.parent):n.children.get(name);Node child=ns.node(id);int start=out.position();
                out.putLong(id).putLong(i+1).putShort((short)length).put((byte)(child.mode>>12)).put(raw).put((byte)0);out.position(start+length);
            }
            Reply reply=new Reply(out.position());reply.data=Arrays.copyOf(out.array(),out.position());return reply;
        }
        case 25: {Reply reply=new Reply(0);reply.fds.add(lockFile(n));return reply;} // duplicate's new lock descriptor
        case 43:n=ns.resolve(p,r.uid,r.gid,r.groups,true);return new Reply(0);
        default:throw new FsException(95);
        }}
    }
    private Reply xattrs(Request r,CeNamespace ns,boolean descriptor,String p,Node n)throws Exception {
        // Serialize the durable locked-root mirror, without holding the
        // namespace monitor while the encrypted snapshot is flushed.
        synchronized(rootAttrs) {
            synchronized(ns) {
                if(!descriptor)n=ns.resolve(p,r.uid,r.gid,r.groups,r.nr!=6 && r.nr!=9 && r.nr!=12 && r.nr!=15);
                boolean set=r.nr<=7,remove=r.nr>=14,list=r.nr>=11&&r.nr<=13;
                if(set || remove) {
                    if(!r.q.startsWith("user."))throw new FsException(95);
                    CeNamespace.access(n,r.uid,r.gid,r.groups,2);
                    if(remove){if(n.xattrs.remove(r.q)==null)throw new FsException(61);}
                    else {
                        int flags=(int)r.a[descriptor?5:4];if((flags&~3)!=0)throw new FsException(22);
                        if((flags&1)!=0 && n.xattrs.containsKey(r.q))throw new FsException(17);
                        if((flags&2)!=0 && !n.xattrs.containsKey(r.q))throw new FsException(61);
                        if(n.xattrs.size()>=64 && !n.xattrs.containsKey(r.q))throw new FsException(7);
                        n.xattrs.put(r.q,r.data.clone());
                    }
                    n.ctime=System.currentTimeMillis();ns.markDirty();
                    if(n.id<3 || n.id>=roots.length+3)return new Reply(0);
                } else {
                    CeNamespace.access(n,r.uid,r.gid,r.groups,4);Reply reply=new Reply(0);
                    if(list) {ByteArrayOutputStream names=new ByteArrayOutputStream();for(String name:n.xattrs.keySet()){names.write(name.getBytes("UTF-8"));names.write(0);}reply.data=names.toByteArray();}
                    else {byte[] value=n.xattrs.get(r.q);if(value==null)throw new FsException(61);reply.data=value.clone();}
                    reply.value=reply.data.length;return reply;
                }
            }
            ns.sync();synchronized(ns){mirrorRoot(n);}return new Reply(0);
        }
    }
    private Reply openRequest(Request r,CeNamespace ns,String p)throws Exception {
        int flags=(int)r.a[2];Node n;
        synchronized(ns) {
            try{n=ns.resolve(p,r.uid,r.gid,r.groups,(flags&OsConstants.O_NOFOLLOW)==0);if((flags&OsConstants.O_CREAT)!=0 && (flags&OsConstants.O_EXCL)!=0)throw new FsException(17);}
            catch(FsException absent){if(absent.errno!=2 || (flags&OsConstants.O_CREAT)==0)throw absent;n=ns.create(p,CeNamespace.REG|((int)r.a[3]&07777),r.uid,r.gid,r.groups,null);}
        }
        return open(n,flags,r.uid,r.gid,r.groups);
    }
    // JNI callbacks run only in the host app. No guest key or plaintext is logged.
    public long fuseSize(int id)throws Exception {CeNamespace ns=unlocked();synchronized(ns){Node n=ns.node(id);return ns.size(n);}}
    public byte[] fuseRead(int id,long offset,int size)throws Exception {
        CeNamespace ns=unlocked();synchronized(ns){byte[] data=new byte[size];int count=ns.store.read(CeNamespace.content(id),offset,data,0,size);return count==size?data:Arrays.copyOf(data,count);}
    }
    public int fuseWrite(int id,long offset,byte[] data)throws Exception {
        CeNamespace ns=unlocked();try{synchronized(ns){ns.store.write(CeNamespace.content(id),offset,data,0,data.length);Node n=ns.node(id);n.mtime=n.ctime=System.currentTimeMillis();ns.markDirty();return data.length;}}
        finally{Arrays.fill(data,(byte)0);}
    }
    public void fuseSync()throws Exception {CeNamespace ns=unlocked();if(!draining && ns.hasPendingChanges())ns.sync();}
    public synchronized void fuseOpened(int id){touched.add(id);opened.put(id,opened.containsKey(id)?opened.get(id)+1:1);}
    private void reclaimClosed(CeNamespace ns,int id)throws Exception {
        boolean live;synchronized(this){live=opened.containsKey(id);}
        if(!live && ns.reclaimFile(id)){synchronized(this){touched.remove(id);}}
    }
    public void fuseRelease(int id)throws Exception {synchronized(this){Integer count=opened.get(id);if(count!=null){if(count==1)opened.remove(id);else opened.put(id,count-1);}}CeNamespace ns=namespace;if(ns!=null && !draining)reclaimClosed(ns,id);}
    synchronized byte[] takePreparedSecret(){byte[] value=preparedSecret;preparedSecret=null;return value;}
    @Override public void close()throws Exception {
        closed=true;
        // Do not interrupt a checkpoint during its atomic durable commit.
        checkpoints.shutdown();boolean interrupted=false;
        while(true)try{if(checkpoints.awaitTermination(1,TimeUnit.DAYS))break;}catch(InterruptedException error){interrupted=true;}
        try {
        try{server.close();}catch(IOException ignored){}try{bound.close();}catch(IOException ignored){}for(LocalSocket peer:peers)try{peer.close();}catch(IOException ignored){}workers.shutdownNow();
        CeFuse current=fuse;
        Exception failure=null;
        try {
            // Called only after the guest process tree exits. Drain cached
            // mmap/write pages into the encrypted store before dropping keys.
            if(current!=null && namespace!=null) {
                Set<Integer> inodes;synchronized(this){inodes=new HashSet<>(touched);}draining=true;
                for(int id:inodes) {
                    try(ParcelFileDescriptor fd=current.open(id,ParcelFileDescriptor.MODE_READ_WRITE)){Os.fsync(fd.getFileDescriptor());}
                    catch(Exception error) {
                        try{namespace.node(id);}catch(FsException gone){if(gone.errno==2)continue;}
                        throw error;
                    }
                }
                namespace.sync();
            }
        }catch(Exception error){failure=error;}
        finally {
            draining=false;fuse=null;if(current!=null)current.close();
            synchronized(this) {
                try {if(namespace!=null)try{namespace.sync();}finally{namespace.store.close();}}
                catch(Exception error){if(failure==null)failure=error;}
                finally{namespace=null;if(diskSecret!=null)Arrays.fill(diskSecret,(byte)0);if(preparedSecret!=null)Arrays.fill(preparedSecret,(byte)0);}
            }
        }
        if(failure!=null)throw failure;
        }finally{if(interrupted)Thread.currentThread().interrupt();}
    }
}
