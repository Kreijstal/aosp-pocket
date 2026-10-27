// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import android.os.*;
import android.system.Os;
import java.io.*;
/** Same-UID Android AppFuse transport; actual encryption stays in CeStorage. */
final class CeFuse implements AutoCloseable {
    static{System.loadLibrary("pocket_ce");}
    private static native long start(int descriptor,Object callbacks);
    private static native void stop(long handle);
    private final IBinder storage;
    private final int mount;
    private final long server;
    private ParcelFileDescriptor keeper;
    final long device;
    CeFuse(Object callbacks)throws Exception {
        if(Build.VERSION.SDK_INT<28 || Build.VERSION.SDK_INT>36)throw new IOException("Unsupported AppFuse host version");
        storage=(IBinder)Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,"mount");
        // Stable explicit AIDL IDs in the pinned Android 9 and Android 16
        // IStorageManager sources. The host service still enforces caller UID.
        Parcel reply=call(74);
        ParcelFileDescriptor bridge;
        try{if(reply.readInt()==0)throw new IOException("AppFuse unavailable");mount=reply.readInt();bridge=reply.readParcelable(null);}
        finally{reply.recycle();}
        server=start(bridge.detachFd(),callbacks);if(server==0)throw new IOException("Cannot start encrypted file transport");
        try{keeper=open(2,ParcelFileDescriptor.MODE_READ_ONLY);device=Os.fstat(keeper.getFileDescriptor()).st_dev;}
        catch(Exception error){stop(server);throw error;}
    }
    private Parcel call(int code,int... args)throws Exception {
        Parcel request=Parcel.obtain(),reply=Parcel.obtain();
        try {
            request.writeInterfaceToken("android.os.storage.IStorageManager");for(int value:args)request.writeInt(value);
            if(!storage.transact(code,request,reply,0))throw new IOException("AppFuse transaction unavailable");reply.readException();return reply;
        }catch(Exception error){reply.recycle();throw error;}finally{request.recycle();}
    }
    ParcelFileDescriptor open(int node,int mode)throws Exception {
        Parcel reply=call(75,mount,node,mode);
        try{if(reply.readInt()==0)throw new IOException("Cannot open encrypted inode");return ParcelFileDescriptor.CREATOR.createFromParcel(reply);}
        finally{reply.recycle();}
    }
    @Override public void close(){try{if(keeper!=null)keeper.close();}catch(IOException ignored){}stop(server);}
}
