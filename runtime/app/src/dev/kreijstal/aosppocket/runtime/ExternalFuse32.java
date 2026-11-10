// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import android.os.*;
import android.system.Os;
import android.system.ErrnoException;
import java.io.*;
import java.util.concurrent.atomic.AtomicBoolean;

/** Kernel file descriptors for the API32 external-storage backend. */
final class ExternalFuse32 implements AutoCloseable {
    // tid is kernel-origin metadata. The native broker must authenticate the
    // known host parent before accepting it as a forwarded guest caller.
    interface Callbacks {
        long fuseSize(int inode,int tid)throws Exception;
        long[] fuseOpen(int inode,int tid,int flags)throws Exception;
        byte[] fuseRead(int inode,long handle,long offset,int size,int tid)throws Exception;
        int fuseWrite(int inode,long handle,long offset,byte[] bytes,int tid)throws Exception;
        void fuseSync(int inode,long handle,int tid,int flags)throws Exception;
        void fuseFlush(int inode,long handle,int tid)throws Exception;
        void fuseRelease(int inode,long handle,int tid,int flags)throws Exception;
    }
    static{System.loadLibrary("pocket_external32");}
    private static native long start(int fd,Object callbacks);
    private static native void stop(long server);
    private static native int[] retireHolders(String root,long device,boolean cachedStat);
    private static native int probeRetirement();
    static void checkRetirementSupport()throws ErrnoException {
        int error=probeRetirement();if(error<0)throw new ErrnoException("external-holder-pidfd",-error);
    }
    private final IBinder storage;
    private final int mount;
    private final long server;
    private final AtomicBoolean closed=new AtomicBoolean();
    private ParcelFileDescriptor keeper;
    final long device;
    ExternalFuse32(Callbacks callbacks)throws Exception {
        if(Build.VERSION.SDK_INT<28 || Build.VERSION.SDK_INT>36)throw new IOException("Unsupported AppFuse host version");
        storage=(IBinder)Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,"mount");
        Parcel reply=call(74);ParcelFileDescriptor bridge;
        try{if(reply.readInt()==0)throw new IOException("AppFuse unavailable");mount=reply.readInt();bridge=reply.readParcelable(null);}
        finally{reply.recycle();}
        server=start(bridge.detachFd(),callbacks);if(server==0)throw new IOException("Cannot start external-file transport");
        try{keeper=openInternal(2,ParcelFileDescriptor.MODE_READ_ONLY);device=Os.fstat(keeper.getFileDescriptor()).st_dev;}
        catch(Exception error){stop(server);throw error;}
    }
    private Parcel call(int code,int... values)throws Exception {
        Parcel request=Parcel.obtain(),reply=Parcel.obtain();
        try {
            request.writeInterfaceToken("android.os.storage.IStorageManager");for(int value:values)request.writeInt(value);
            if(!storage.transact(code,request,reply,0))throw new IOException("AppFuse transaction unavailable");reply.readException();return reply;
        }catch(Exception error){reply.recycle();throw error;}finally{request.recycle();}
    }
    private ParcelFileDescriptor openInternal(int node,int mode)throws Exception {
        Parcel reply=call(75,mount,node,mode);
        try{if(reply.readInt()==0)throw new IOException("Cannot open external-file proxy");return ParcelFileDescriptor.CREATOR.createFromParcel(reply);}
        finally{reply.recycle();}
    }
    ParcelFileDescriptor open(int node,int mode)throws Exception {
        if(closed.get() || node<3)throw new IOException("External-file transport is closed or invalid");
        if(mode!=ParcelFileDescriptor.MODE_READ_ONLY && mode!=ParcelFileDescriptor.MODE_WRITE_ONLY && mode!=ParcelFileDescriptor.MODE_READ_WRITE)
            throw new IOException("Invalid external-file access mode");
        return openInternal(node,mode);
    }
    int[] retireHolders(File root)throws Exception {
        if(closed.get())throw new IOException("External-file transport is closed");
        int[] retired=retireHolders(root.getPath(),device,Build.VERSION.SDK_INT>=30);if(retired==null)throw new IOException("Cannot retire external-volume holders");return retired;
    }
    @Override public void close() {
        if(!closed.compareAndSet(false,true))return;
        try{if(keeper!=null)keeper.close();}catch(IOException ignored){}
        stop(server);
    }
}
