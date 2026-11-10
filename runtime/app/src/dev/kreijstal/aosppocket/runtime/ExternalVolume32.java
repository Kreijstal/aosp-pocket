// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import android.net.*;
import android.os.Process;
import android.system.*;
import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;
import org.json.*;

/** Session-scoped real AppFuse adapter for the trusted native storage core. */
final class ExternalVolume32 implements AutoCloseable {
    private static final int MAGIC=0x32565850;
    private final File root,endpoint;
    private final LocalSocket binding=new LocalSocket();
    private final LocalServerSocket server;
    private final Thread worker;
    private final Set<LocalSocket> peers=java.util.concurrent.ConcurrentHashMap.newKeySet();
    private ExternalFiles32 files;
    private int broker;
    private long epoch;
    private volatile boolean closed;
    private final java.util.concurrent.atomic.AtomicBoolean stopping=new java.util.concurrent.atomic.AtomicBoolean();
    private volatile Exception failure;
    private int attaches,revocations,denials,replyFailures;
    private String lastReplyFailure;
    private final JSONArray retiredHolders=new JSONArray();
    static boolean selected(File root) {return new File(root,"system/etc/pocket-external-volume32").isFile();}
    private static void clearStoppedEndpoints(File root)throws Exception {
        File[] identities=new File(root,".pocket-identity").listFiles();
        if(identities!=null)for(File identity:identities)if(identity.getName().matches("[1-9][0-9]*")) {
            int pid;try{pid=Integer.parseInt(identity.getName());}catch(NumberFormatException large){continue;}
            if(HostDns.trustedPeer(root,pid,0))throw new IOException("A live guest owner still holds external storage");
        }
        for(String name:new String[]{"dev/socket/pocket_volume32","dev/socket/pocket_ext","dev/socket/pocket_efs","dev/socket/pocket_xfd",".pocket-external-ready",".pocket-external-device"}) {
            File file=new File(root,name);if(!Files.exists(file.toPath(),LinkOption.NOFOLLOW_LINKS))continue;
            int kind=Os.lstat(file.getPath()).st_mode&OsConstants.S_IFMT,expected=name.startsWith("dev/")?OsConstants.S_IFSOCK:OsConstants.S_IFREG;
            if(kind!=expected)throw new IOException("Unexpected stale external-storage file type: "+name);
            Files.delete(file.toPath());
        }
    }
    ExternalVolume32(File root)throws Exception {
        this.root=root;endpoint=new File(root,"dev/socket/pocket_volume32");
        if(!selected(root))throw new IOException("Image did not select the external-volume backend");
        // DirectSession has stopped/reaped the previous guest. Independently
        // reject live UID0 records before removing only known stale sockets
        // and metadata left by a killed outer process.
        clearStoppedEndpoints(root);
        // These selectors must precede the first guest interpreter/filter.
        // They persist across this profile's boots; no live marker toggling.
        for(String marker:new String[]{".pocket-external-enabled",".pocket-external-namespace-enabled"}) {
            Path path=new File(root,marker).toPath();
            if(Files.exists(path,LinkOption.NOFOLLOW_LINKS)){if(!Files.isRegularFile(path,LinkOption.NOFOLLOW_LINKS))throw new IOException("Invalid namespace selector");}
            else Files.createFile(path);
        }
        binding.bind(new LocalSocketAddress(endpoint.getPath(),LocalSocketAddress.Namespace.FILESYSTEM));
        server=new LocalServerSocket(binding.getFileDescriptor());Os.chmod(endpoint.getPath(),0600);
        worker=new Thread(this::accept,"PocketExternalVolume32");worker.start();
    }
    private void accept() {
        while(!closed)try {
            LocalSocket socket=server.accept();peers.add(socket);
            try{serve(socket);}finally{peers.remove(socket);socket.close();}
        }catch(Exception error){if(!closed)failure=error;}
    }
    private boolean trusted(Credentials peer) {
        if(peer.getUid()!=Process.myUid() || !HostDns.trustedPeer(root,peer.getPid(),0))return false;
        try {
            byte[] command=Files.readAllBytes(Paths.get("/proc/"+peer.getPid()+"/cmdline"));int end=0;while(end<command.length && command[end]!=0)end++;
            String executable=new String(command,0,end,"UTF-8");
            return executable.equals("/system/bin/pocket-core-api32") || executable.equals(new File(root,"system/bin/pocket-core-api32").getPath());
        }catch(IOException unavailable){return false;}
    }
    private synchronized void serve(LocalSocket socket)throws Exception {
        socket.setSoTimeout(20000);Credentials peer=socket.getPeerCredentials();
        if(!trusted(peer)){denials++;return;}
        byte[] bytes=new byte[24];int at=0;boolean rights=false;
        while(at<bytes.length) {
            int count=socket.getInputStream().read(bytes,at,bytes.length-at);if(count<=0)throw new EOFException("Incomplete volume request");at+=count;
            FileDescriptor[] descriptors=socket.getAncillaryFileDescriptors();if(descriptors!=null)for(FileDescriptor fd:descriptors){rights=true;Os.close(fd);}
        }
        ByteBuffer request=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        int magic=request.getInt(),version=request.getInt(),operation=request.getInt(),reserved=request.getInt();long nonce=request.getLong();
        int error=0;
        try {
            if(closed || rights || magic!=MAGIC || version!=1 || reserved!=0 || nonce==0 || (operation!=1 && operation!=2))throw new ErrnoException("volume-frame",OsConstants.EINVAL);
            if(operation==1) {
                if(files!=null) {
                    if(broker!=peer.getPid() || epoch!=nonce)throw new ErrnoException("volume-epoch",OsConstants.EBUSY);
                }else {
                    ExternalFiles32 next=new ExternalFiles32(root,peer.getPid());
                    try{next.serveOpens();}catch(Exception problem){try{next.revoke();}catch(Exception cleanup){problem.addSuppressed(cleanup);}throw problem;}
                    files=next;broker=peer.getPid();epoch=nonce;attaches++;
                }
            }else if(files!=null) {
                if(broker!=peer.getPid() || epoch!=nonce)throw new ErrnoException("volume-epoch",OsConstants.ESTALE);
                files.revoke();for(int pid:files.retiredHolders())retiredHolders.put(pid);revocations++;
                files=null;broker=0;epoch=0;
            }
        }catch(Exception problem){error=problem instanceof ErrnoException?-((ErrnoException)problem).errno:-OsConstants.EIO;failure=problem;}
        // The adapter remains owned if the reply is lost. Native detach is
        // idempotent and carries the same random epoch on its recovery path.
        try(android.os.ParcelFileDescriptor lifetime=android.os.ParcelFileDescriptor.open(new File("/proc/"+Process.myPid()+"/stat"),android.os.ParcelFileDescriptor.MODE_READ_ONLY)) {
            try {
                if(error==0)socket.setFileDescriptorsForSend(new FileDescriptor[]{lifetime.getFileDescriptor()});
                socket.getOutputStream().write(ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN).putInt(MAGIC).putInt(1).putInt(error).putInt(0).putLong(nonce).array());
            }catch(IOException lostReply){replyFailures++;lastReplyFailure=lostReply.toString();}
            finally{socket.setFileDescriptorsForSend(null);}
        }
    }
    synchronized JSONObject report()throws Exception {
        JSONObject result=new JSONObject().put("attached",files!=null).put("attaches",attaches).put("revocations",revocations).put("denials",denials).put("workerStopped",!worker.isAlive())
            .put("epoch",epoch).put("retiredHolders",new JSONArray(retiredHolders.toString())).put("replyFailures",replyFailures);
        if(lastReplyFailure!=null)result.put("lastReplyFailure",lastReplyFailure);
        if(files!=null)result.put("files",files.report());if(failure!=null)result.put("lastError",failure.toString());return result;
    }
    @Override public void close()throws Exception {
        if(!stopping.compareAndSet(false,true))return;
        closed=true;
        try{Os.shutdown(binding.getFileDescriptor(),OsConstants.SHUT_RDWR);}catch(ErrnoException stopped){if(stopped.errno!=OsConstants.ENOTCONN && stopped.errno!=OsConstants.EBADF)throw stopped;}
        for(LocalSocket peer:peers)try{peer.close();}catch(IOException ignored){}
        server.close();binding.close();worker.join(25000);
        if(worker.isAlive())throw new IOException("External-volume control worker did not stop");
        try{synchronized(this){if(files!=null){try{files.revoke();}finally{files=null;broker=0;epoch=0;}}}}
        finally{Files.deleteIfExists(endpoint.toPath());}
    }
}
