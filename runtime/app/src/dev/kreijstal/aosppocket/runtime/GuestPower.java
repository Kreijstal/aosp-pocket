// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.content.Context;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.PowerManager;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

/** App-owned CPU wake locks for the guest's suspend HAL; host suspend policy stays with Android. */
public final class GuestPower implements AutoCloseable {
    private static final int MAGIC=0x504b5031;
    private final PowerManager manager;
    private final LocalSocket listener=new LocalSocket();
    private final LocalServerSocket server;
    private final File path;
    private final Map<Long,PowerManager.WakeLock> locks=new HashMap<>();
    private final Thread worker;
    private long nextId=1;
    private volatile boolean closed;

    public GuestPower(Context context,File root)throws IOException {
        manager=(PowerManager)context.getSystemService(Context.POWER_SERVICE);
        path=new File(root,"dev/socket/pocket_power");
        if(path.exists() && !path.delete())throw new IOException("Cannot remove stale guest power socket");
        listener.bind(new LocalSocketAddress(path.getPath(),LocalSocketAddress.Namespace.FILESYSTEM));
        server=new LocalServerSocket(listener.getFileDescriptor());
        worker=new Thread(()->{
            while(!closed) {
                try(LocalSocket client=server.accept()) {
                    client.setSoTimeout(2000);
                    if(client.getPeerCredentials().getUid()!=android.os.Process.myUid())continue;
                    DataInputStream in=new DataInputStream(client.getInputStream());
                    DataOutputStream out=new DataOutputStream(client.getOutputStream());
                    if(in.readInt()!=MAGIC)continue;
                    int operation=in.readInt(),length=in.readInt();long token=in.readLong();
                    if(length<0 || length>256)continue;
                    byte[] name=new byte[length];in.readFully(name);
                    long answer=0;int status=0;
                    synchronized(locks) {
                        if(closed)status=5;
                        else if(operation==1 && length>0) {
                            if(locks.size()>=128)status=28;
                            else {
                                PowerManager.WakeLock lock=manager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,
                                    "Pocket:"+new String(name,StandardCharsets.UTF_8));
                                lock.setReferenceCounted(false);
                                try{lock.acquire();answer=nextId++;locks.put(answer,lock);}
                                catch(SecurityException denied){status=13;}
                            }
                        }else if(operation==2 && length==0) {
                            PowerManager.WakeLock lock=locks.remove(token);
                            if(lock!=null && lock.isHeld())lock.release();
                        }else if(operation==3 && length==0) {
                            for(PowerManager.WakeLock lock:locks.values())if(lock.isHeld())answer++;
                        }
                        else status=22;
                    }
                    out.writeInt(status);out.writeLong(answer);out.flush();
                }catch(IOException error){if(!closed)android.util.Log.w("PocketPower","Power request failed",error);}
            }
        },"PocketGuestPower");
        worker.start();
    }
    @Override public void close() {
        closed=true;
        try{server.close();}catch(IOException ignored){}
        try{listener.close();}catch(IOException ignored){}
        synchronized(locks) {
            for(PowerManager.WakeLock lock:locks.values())if(lock.isHeld())lock.release();
            locks.clear();
        }
        path.delete();
    }
}
