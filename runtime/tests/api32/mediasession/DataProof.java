// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketmediasession32;
import android.net.*;
import android.os.*;
import android.system.*;
import java.io.*;
import java.lang.reflect.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import org.json.*;

/** Real AppFuse FDs consumed by an ordinary guest process and worker thread. */
final class DataProof {
    private static void check(boolean value,String reason)throws IOException{if(!value)throw new IOException(reason);}
    static JSONObject run(String nonce)throws Exception {
        if(new File("/.pocket-external-enabled").isFile())return PathProof.run(nonce);
        JSONObject result=new JSONObject();
        // An app with the same real host UID still cannot impersonate the
        // host-parent callback adapter by supplying its own tid/UID fields.
        try(LocalSocket forged=new LocalSocket()) {
            forged.connect(new LocalSocketAddress("/dev/socket/pocket_ext",LocalSocketAddress.Namespace.FILESYSTEM));forged.setSoTimeout(5000);
            byte[] reply=new byte[32];new DataInputStream(forged.getInputStream()).readFully(reply);
            check(ByteBuffer.wrap(reply).order(ByteOrder.LITTLE_ENDIAN).getInt()==-OsConstants.EACCES,"App reached host-only file callbacks");result.put("hostRpcImpersonationDenied",true);
        }
        try(LocalSocket bound=new LocalSocket()) {
            bound.bind(new LocalSocketAddress("/dev/socket/pme32",LocalSocketAddress.Namespace.FILESYSTEM));
            try(LocalServerSocket server=new LocalServerSocket(bound.getFileDescriptor());LocalSocket host=server.accept()) {
                int parent=Integer.parseInt(System.getenv("POCKET_PARENT_PID"));check(parent>0 && host.getPeerCredentials().getPid()==parent,"Unexpected proxy descriptor sender");
                host.setSoTimeout(20000);check(host.getInputStream().read()=='F',"Missing host proxy descriptors");FileDescriptor[] fds=host.getAncillaryFileDescriptors();check(fds!=null && fds.length==2,"Expected two real descriptors");
                try(ParcelFileDescriptor cached=ParcelFileDescriptor.dup(fds[0]);ParcelFileDescriptor direct=ParcelFileDescriptor.dup(fds[1])) {
                    for(FileDescriptor fd:fds)Os.close(fd);
                    FileDescriptor rw=cached.getFileDescriptor(),ro=direct.getFileDescriptor();byte[] data=new byte[64];
                    String expected="pocket-real-media-"+nonce;check(Os.pread(rw,data,0,expected.length(),0)==expected.length() && expected.equals(new String(data,0,expected.length(),"UTF-8")),"Provider/AppFuse initial read differs");
                    byte[] update="proxy-write".getBytes("UTF-8");check(Os.pwrite(rw,update,0,update.length,128)==update.length,"Provider write did not finish");
                    long mapping=Os.mmap(0,8192,OsConstants.PROT_READ|OsConstants.PROT_WRITE,OsConstants.MAP_SHARED,rw,0);
                    try {
                        byte[] mapped="shared-media-map".getBytes("UTF-8");Class<?> memory=Class.forName("libcore.io.Memory");memory.getMethod("pokeByteArray",long.class,byte[].class,int.class,int.class).invoke(null,mapping+256,mapped,0,mapped.length);
                        Os.msync(mapping,8192,OsConstants.MS_SYNC);
                    }finally{Os.munmap(mapping,8192);}
                    Os.fdatasync(rw);Os.fsync(rw);result.put("mainTid",android.os.Process.myTid());
                    FutureTask<Integer> read=new FutureTask<>(()->{byte[] value=new byte[11];check(Os.pread(ro,value,0,1,4096)==1 && value[0]=='Q',"Worker direct read differs");
                        check(Os.pread(ro,value,0,value.length,128)==value.length && new String(value,"UTF-8").equals("proxy-write"),"Independent open did not see synced writes");return android.os.Process.myTid();});
                    Thread worker=new Thread(read,"MediaFdWorker");worker.start();result.put("workerTid",read.get(10,TimeUnit.SECONDS));worker.join();
                    try{Os.pwrite(ro,update,0,update.length,0);throw new IOException("Read-only descriptor accepted write");}catch(ErrnoException expectedError){check(expectedError.errno==OsConstants.EBADF,"Read-only descriptor errno changed");}
                    result.put("uid",Os.getuid()).put("readWrite",true).put("sharedMmap",true).put("readOnlyDenied",true);
                }
                host.getOutputStream().write('D');
            }
        }
        long deadline=SystemClock.elapsedRealtime()+30000;File done=new File("/tmp/media-proxy-done");
        while(!done.isFile()){check(SystemClock.elapsedRealtime()<deadline,"Host did not release external files");Thread.sleep(20);}
        check(nonce.equals(new String(Files.readAllBytes(done.toPath()),"US-ASCII")),"Wrong proxy completion nonce");
        return result;
    }
}
