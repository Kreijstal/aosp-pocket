// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import android.net.*;
import android.os.*;
import android.system.*;
import java.io.*;
import java.nio.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import org.json.*;

/** Host AppFuse callbacks for already-authorized native32 file capabilities. */
final class ExternalFiles32 implements ExternalFuse32.Callbacks,AutoCloseable {
    private static final int MAGIC=0x32465850;
    private static final class Slot {
        final long cookie;final int flags,mode;final AtomicBoolean released=new AtomicBoolean();
        Slot(long cookie,int flags,int mode){this.cookie=cookie;this.flags=flags;this.mode=mode;}
    }
    private static final class Reply {long value;int uid,tid,flags;byte[] data;}
    private final String endpoint;
    private final int brokerPid;
    private final ExternalFuse32 transport;
    private final ConcurrentMap<Integer,Slot> slots=new ConcurrentHashMap<>();
    private final List<ParcelFileDescriptor> owned=new ArrayList<>();
    private final Set<Integer> readTids=ConcurrentHashMap.newKeySet(),callerUids=ConcurrentHashMap.newKeySet();
    private final AtomicInteger fallbacks=new AtomicInteger(),releases=new AtomicInteger();
    private final AtomicInteger openRequests=new AtomicInteger(),openPeerDenials=new AtomicInteger();
    private final Set<LocalSocket> openPeers=ConcurrentHashMap.newKeySet();
    private final File attachEndpoint;
    private final File deviceRecord;
    private final File root;
    private int[] retiredHolders=new int[0];
    private boolean ownsDeviceRecord;
    private LocalSocket openBinding;
    private LocalServerSocket openServer;
    private Thread openAcceptor;
    private ThreadPoolExecutor openWorkers;
    private volatile Exception releaseError;
    private volatile boolean closed;
    private boolean revoked,opensStopped;
    ExternalFiles32(File root,int brokerPid)throws Exception {
        if(brokerPid<=0)throw new IOException("Invalid external-file broker");
        this.root=root;this.brokerPid=brokerPid;endpoint=new File(root,"dev/socket/pocket_ext").getPath();attachEndpoint=new File(root,"dev/socket/pocket_xfd");deviceRecord=new File(root,".pocket-external-device");transport=new ExternalFuse32(this);
    }
    private Slot slot(int inode,long cookie)throws ErrnoException {
        Slot entry=slots.get(inode);if(entry==null || entry.cookie!=cookie || entry.released.get())throw new ErrnoException("external-file",OsConstants.EBADF);return entry;
    }
    private Reply call(int op,int inode,long cookie,long offset,int size,int tid,int flags,byte[] body)throws Exception {
        try(LocalSocket socket=new LocalSocket();ParcelFileDescriptor lifetime=ParcelFileDescriptor.open(new File("/proc/"+android.os.Process.myPid()+"/stat"),ParcelFileDescriptor.MODE_READ_ONLY)) {
            socket.connect(new LocalSocketAddress(endpoint,LocalSocketAddress.Namespace.FILESYSTEM));socket.setSoTimeout(15000);
            Credentials peer=socket.getPeerCredentials();if(peer.getPid()!=brokerPid || peer.getUid()!=android.os.Process.myUid())throw new IOException("External-file broker identity changed");
            ByteBuffer request=ByteBuffer.allocate(48).order(ByteOrder.LITTLE_ENDIAN);
            request.putInt(MAGIC).putInt(1).putInt(op).putInt(inode).putLong(cookie).putLong(offset).putInt(size).putInt(tid).putInt(flags).putInt(0);
            OutputStream out=socket.getOutputStream();socket.setFileDescriptorsForSend(new FileDescriptor[]{lifetime.getFileDescriptor()});
            out.write(0);socket.setFileDescriptorsForSend(null);out.write(request.array());if(body!=null)out.write(body);
            DataInputStream in=new DataInputStream(socket.getInputStream());byte[] header=new byte[32];in.readFully(header);ByteBuffer response=ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
            int error=response.getInt(),count=response.getInt();Reply reply=new Reply();reply.value=response.getLong();reply.uid=response.getInt();reply.tid=response.getInt();reply.flags=response.getInt();int reserved=response.getInt();
            if(count<0 || count>131072 || reserved!=0 || (error!=0 && count!=0) || (op!=2 && count!=0) || (op==2 && count>size))throw new IOException("Invalid external-file reply");
            if(error<0 && error>=-4095)throw new ErrnoException("external-file",-error);
            if(error!=0)throw new IOException("Invalid external-file errno");
            reply.data=new byte[count];in.readFully(reply.data);return reply;
        }
    }
    synchronized ParcelFileDescriptor attach(int inode,long cookie,int flags,int mode)throws Exception {
        if(closed || inode<3 || cookie==0 || (flags&~3)!=0 || (mode!=ParcelFileDescriptor.MODE_READ_ONLY && mode!=ParcelFileDescriptor.MODE_READ_WRITE && mode!=ParcelFileDescriptor.MODE_WRITE_ONLY))throw new IOException("Invalid external-file capability");
        Slot entry=new Slot(cookie,flags,mode);if(slots.putIfAbsent(inode,entry)!=null)throw new IOException("External-file slot already attached");
        try{ParcelFileDescriptor fd=transport.open(inode,mode);owned.add(fd);return fd;}
        catch(Exception error){if(entry.released.compareAndSet(false,true))try{call(6,inode,cookie,0,0,0,0,null);}catch(Exception cleanup){error.addSuppressed(cleanup);}slots.remove(inode,entry);throw error;}
    }
    synchronized void serveOpens()throws Exception {
        if(closed || openAcceptor!=null)throw new IOException("External-file attach server already used");
        if(java.nio.file.Files.exists(attachEndpoint.toPath(),java.nio.file.LinkOption.NOFOLLOW_LINKS))throw new IOException("External-file attach endpoint already exists");
        if(java.nio.file.Files.exists(deviceRecord.toPath(),java.nio.file.LinkOption.NOFOLLOW_LINKS))throw new IOException("External-file device record already exists");
        openWorkers=new ThreadPoolExecutor(4,4,0,TimeUnit.SECONDS,new ArrayBlockingQueue<>(64));
        try {
            openBinding=new LocalSocket();openBinding.bind(new LocalSocketAddress(attachEndpoint.getPath(),LocalSocketAddress.Namespace.FILESYSTEM));
            openServer=new LocalServerSocket(openBinding.getFileDescriptor());Os.chmod(attachEndpoint.getPath(),0600);
            java.nio.file.Files.createFile(deviceRecord.toPath());ownsDeviceRecord=true;
            java.nio.file.Files.write(deviceRecord.toPath(),ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(transport.device).array());Os.chmod(deviceRecord.getPath(),0600);
            openAcceptor=new Thread(()->{
                while(!closed)try {
                    LocalSocket peer=openServer.accept();if(closed){peer.close();break;}openPeers.add(peer);
                    try{openWorkers.execute(()->serveOpen(peer));}
                    catch(RejectedExecutionException full){openPeers.remove(peer);peer.close();}
                }catch(IOException stopped){break;}
            },"PocketExternalOpen32");openAcceptor.start();
        }catch(Exception error){stopOpens();throw error;}
    }
    private void serveOpen(LocalSocket peer) {
        try(LocalSocket socket=peer) {
            socket.setSoTimeout(15000);Credentials credentials=socket.getPeerCredentials();
            if(credentials.getUid()!=android.os.Process.myUid() || credentials.getPid()!=brokerPid) {
                openPeerDenials.incrementAndGet();socket.getOutputStream().write(ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putInt(-OsConstants.EACCES).putInt(0).array());return;
            }
            byte[] header=new byte[32];new DataInputStream(socket.getInputStream()).readFully(header);ByteBuffer request=ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
            int magic=request.getInt(),version=request.getInt(),inode=request.getInt(),flags=request.getInt();long cookie=request.getLong();int access=request.getInt(),reserved=request.getInt();
            if(magic!=0x32415850 || version!=1 || inode<3 || cookie==0 || (flags&~3)!=0 || access<0 || access>2 || reserved!=0)throw new IOException("Invalid external-file attach request");
            int mode=access==0?ParcelFileDescriptor.MODE_READ_ONLY:access==1?ParcelFileDescriptor.MODE_WRITE_ONLY:ParcelFileDescriptor.MODE_READ_WRITE;
            ParcelFileDescriptor file=null;
            try {
                // A complete valid attach request transfers capability cleanup
                // to this adapter. attach() releases it if proxy creation fails.
                file=attach(inode,cookie,flags,mode);
                try(ParcelFileDescriptor lifetime=ParcelFileDescriptor.open(new File("/proc/"+android.os.Process.myPid()+"/stat"),ParcelFileDescriptor.MODE_READ_ONLY)) {
                    socket.setFileDescriptorsForSend(new FileDescriptor[]{file.getFileDescriptor(),lifetime.getFileDescriptor()});
                    socket.getOutputStream().write(ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN).putInt(0).putInt(0).putLong(transport.device).putLong(inode).array());socket.setFileDescriptorsForSend(null);openRequests.incrementAndGet();
                }
            }catch(Exception error) {
                // close() can reject attach before it enters the slot map.
                // The trusted broker already transferred this cap to us.
                if(file==null)try{call(6,inode,cookie,0,0,0,0,null);}catch(Exception alreadyReleased){}
                int errno=error instanceof ErrnoException?((ErrnoException)error).errno:OsConstants.EIO;
                try{socket.setFileDescriptorsForSend(null);socket.getOutputStream().write(ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN).putInt(-errno).array());}catch(IOException disconnected){}
            }finally {
                if(file!=null){synchronized(this){owned.remove(file);}file.close();}
            }
        }catch(Exception disconnected){}finally{openPeers.remove(peer);}
    }
    private void stopOpens()throws Exception {
        synchronized(this){if(opensStopped)return;}
        Exception failure=null;
        // close alone need not wake a thread blocked in accept on Linux.
        // Shut the listener down while its descriptor is still owned/live.
        if(openBinding!=null)try{Os.shutdown(openBinding.getFileDescriptor(),OsConstants.SHUT_RDWR);}
        catch(ErrnoException error){if(error.errno!=OsConstants.ENOTCONN && error.errno!=OsConstants.EBADF)failure=error;}
        if(openServer!=null)try{openServer.close();}catch(IOException error){failure=error;}
        if(openBinding!=null)try{openBinding.close();}catch(IOException ignored){}
        for(LocalSocket socket:openPeers)try{socket.close();}catch(IOException ignored){}
        if(openAcceptor!=null){openAcceptor.join(2000);if(openAcceptor.isAlive())failure=new IOException("External-file accept thread did not stop");}
        if(openWorkers!=null){openWorkers.shutdown();if(!openWorkers.awaitTermination(20000,TimeUnit.MILLISECONDS))failure=new IOException("External-file attach workers did not stop");}
        if(openServer!=null)java.nio.file.Files.deleteIfExists(attachEndpoint.toPath());
        if(failure!=null)throw failure;
        synchronized(this){opensStopped=true;}
    }
    public long fuseSize(int inode,int tid)throws Exception {
        Slot entry=slots.get(inode);if(entry==null)throw new ErrnoException("external-file",OsConstants.ENOENT);
        long size=call(1,inode,entry.cookie,0,0,tid,0,null).value;if(size<0)throw new IOException("External-file size overflow");return size;
    }
    public long[] fuseOpen(int inode,int tid,int flags)throws Exception {
        Slot entry=slots.get(inode);if(entry==null || entry.released.get())throw new ErrnoException("external-file",OsConstants.EBADF);
        int expected=entry.mode==ParcelFileDescriptor.MODE_READ_ONLY?OsConstants.O_RDONLY:entry.mode==ParcelFileDescriptor.MODE_WRITE_ONLY?OsConstants.O_WRONLY:OsConstants.O_RDWR;
        if((flags&OsConstants.O_ACCMODE)!=expected)throw new ErrnoException("external-file",OsConstants.EACCES);
        return new long[]{entry.cookie,entry.flags};
    }
    private void observe(Reply reply,int tid,boolean read) {callerUids.add(reply.uid);if(read && tid>0)readTids.add(tid);if((reply.flags&1)!=0)fallbacks.incrementAndGet();}
    public byte[] fuseRead(int inode,long cookie,long offset,int size,int tid)throws Exception {
        slot(inode,cookie);Reply reply=call(2,inode,cookie,offset,size,tid,0,null);observe(reply,tid,true);return reply.data;
    }
    public int fuseWrite(int inode,long cookie,long offset,byte[] bytes,int tid)throws Exception {
        slot(inode,cookie);Reply reply=call(3,inode,cookie,offset,bytes.length,tid,0,bytes);observe(reply,tid,false);
        if(reply.value<0 || reply.value>bytes.length)throw new IOException("External-file write bound");return (int)reply.value;
    }
    public void fuseSync(int inode,long cookie,int tid,int flags)throws Exception {slot(inode,cookie);Reply reply=call(4,inode,cookie,0,0,tid,flags,null);observe(reply,tid,false);}
    public void fuseFlush(int inode,long cookie,int tid)throws Exception {slot(inode,cookie);call(5,inode,cookie,0,0,tid,0,null);}
    public void fuseRelease(int inode,long cookie,int tid,int flags)throws Exception {
        Slot entry=slots.get(inode);if(entry==null || entry.cookie!=cookie)throw new ErrnoException("external-file",OsConstants.EBADF);
        if(!entry.released.compareAndSet(false,true))return;
        try{call(6,inode,cookie,0,0,tid,0,null);releases.incrementAndGet();}
        catch(Exception error){releaseError=error;throw error;}
        finally{slots.remove(inode,entry);}
    }
    JSONObject report()throws Exception {
        return new JSONObject().put("readTids",new JSONArray(readTids)).put("callerUids",new JSONArray(callerUids)).put("capabilityFallbackCalls",fallbacks.get()).put("releasedFiles",releases.get()).put("remainingSlots",slots.size()).put("openRequests",openRequests.get()).put("openPeerDenials",openPeerDenials.get()).put("acceptThreadStopped",openAcceptor==null || !openAcceptor.isAlive()).put("attachWorkersStopped",openWorkers==null || openWorkers.isTerminated());
    }
    int[] retiredHolders(){return retiredHolders.clone();}
    // Volume unmount revokes the kernel data plane before releasing remaining
    // native caps. Native stop joins all callback threads, so a late kernel
    // RELEASE cannot race this sweep or release a capability twice. The normal
    // close() below remains strict for clients expected to close their FDs.
    void revoke()throws Exception {
        synchronized(this){if(revoked)return;closed=true;}Exception failure=null;
        android.util.Log.i("PocketExternal32","revoke stage=begin");
        try {
            try{stopOpens();}catch(Exception error){failure=error;}
            android.util.Log.i("PocketExternal32","revoke stage=opens_stopped");
            // AOSP vold retires volume users before unmount. Cached reads and
            // existing mappings can outlive FUSE disconnect, so closing only
            // the adapter is insufficient. Native code scopes pidfd signals to
            // verified profile identities holding this exact kernel device.
            retiredHolders=transport.retireHolders(root);
            android.util.Log.i("PocketExternal32","revoke stage=holders_retired");
            synchronized(this){for(ParcelFileDescriptor fd:owned)try{fd.close();}catch(Exception error){if(failure==null)failure=error;}owned.clear();}
            android.util.Log.i("PocketExternal32","revoke stage=owned_files_closed");
            transport.close();
            android.util.Log.i("PocketExternal32","revoke stage=transport_stopped");
            for(Map.Entry<Integer,Slot> item:slots.entrySet()) {
                Slot entry=item.getValue();if(!entry.released.compareAndSet(false,true))continue;
                try{call(6,item.getKey(),entry.cookie,0,0,0,0,null);releases.incrementAndGet();}
                catch(Exception error){if(failure==null)failure=error;}
                finally{slots.remove(item.getKey(),entry);}
            }
            if(releaseError!=null && failure==null)failure=releaseError;
            android.util.Log.i("PocketExternal32","revoke stage=capabilities_released");
        }finally{transport.close();if(ownsDeviceRecord){java.nio.file.Files.deleteIfExists(deviceRecord.toPath());ownsDeviceRecord=false;}}
        android.util.Log.i("PocketExternal32","revoke stage=complete");
        if(failure!=null)throw failure;
        synchronized(this){revoked=true;}
    }
    @Override public void close()throws Exception {
        synchronized(this){if(closed)return;closed=true;}Exception failure=null;
        try {
            try{stopOpens();}catch(Exception error){failure=error;}
            for(ParcelFileDescriptor fd:owned)try{fd.close();}catch(Exception error){if(failure==null)failure=error;}owned.clear();
            long deadline=SystemClock.elapsedRealtime()+10000;
            while(!slots.isEmpty() && SystemClock.elapsedRealtime()<deadline)Thread.sleep(10);
            if(!slots.isEmpty())failure=new IOException("External files still held after proxy shutdown");
            if(releaseError!=null)failure=releaseError;
        }finally{transport.close();if(ownsDeviceRecord){java.nio.file.Files.deleteIfExists(deviceRecord.toPath());ownsDeviceRecord=false;}}
        if(failure!=null)throw failure;
    }
}
