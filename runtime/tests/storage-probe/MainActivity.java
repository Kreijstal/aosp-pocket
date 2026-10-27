// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.storageprobe;

import android.app.Activity;
import android.os.*;
import android.os.storage.StorageManager;
import android.system.Os;
import android.widget.TextView;
import java.io.*;
import java.lang.reflect.*;
import org.json.JSONObject;
import dev.kreijstal.aosppocket.storage.EncryptedStore;

/** Test only: anonymous scratch files; never opens or migrates Pocket userdata. */
public final class MainActivity extends Activity {
    static {System.loadLibrary("pocketstorageprobe");}
    private static native long startServer(int fd);
    private static native void stopServer(long server);
    private static native String testDescriptors(int fd,int second);
    private JSONObject encryptedProxy(StorageManager storage,java.nio.file.Path parent)throws Exception {
        byte[] secret=new byte[32];new java.security.SecureRandom().nextBytes(secret);
        java.nio.file.Path directory=parent.resolve("mapped-ciphertext");
        HandlerThread thread=new HandlerThread("EncryptedProxyCallbacks");thread.start();
        final java.util.concurrent.CountDownLatch released=new java.util.concurrent.CountDownLatch(1);
        JSONObject result;
        try {
            try(final EncryptedStore store=EncryptedStore.create(directory,secret)) {
                store.createFile("mapped-file");byte[] initial=new byte[65536];java.util.Arrays.fill(initial,(byte)0x3d);
                store.write("mapped-file",0,initial,0,initial.length);store.sync();
                ProxyFileDescriptorCallback callback=new ProxyFileDescriptorCallback() {
                    private android.system.ErrnoException error(Exception cause) {
                        return new android.system.ErrnoException("encrypted proxy",android.system.OsConstants.EIO,cause);
                    }
                    @Override public long onGetSize()throws android.system.ErrnoException {
                        try{return store.size("mapped-file");}catch(Exception cause){throw error(cause);}
                    }
                    @Override public int onRead(long offset,int size,byte[] data)throws android.system.ErrnoException {
                        try{return store.read("mapped-file",offset,data,0,size);}catch(Exception cause){throw error(cause);}
                    }
                    @Override public int onWrite(long offset,int size,byte[] data)throws android.system.ErrnoException {
                        try{store.write("mapped-file",offset,data,0,size);return size;}catch(Exception cause){throw error(cause);}
                    }
                    @Override public void onFsync()throws android.system.ErrnoException {
                        try{store.sync();}catch(Exception cause){throw error(cause);}
                    }
                    @Override public void onRelease(){released.countDown();}
                };
                try(ParcelFileDescriptor fd=storage.openProxyFileDescriptor(ParcelFileDescriptor.MODE_READ_WRITE,callback,new Handler(thread.getLooper()))) {
                    result=new JSONObject(testDescriptors(fd.getFd(),-1));
                }
                if(!released.await(10,java.util.concurrent.TimeUnit.SECONDS))throw new IOException("Encrypted proxy release timed out");
                if(result.getInt("readMmapErrno")!=0 || !result.getBoolean("initialContentMatches") ||
                    result.getInt("sharedMmapErrno")!=0 || result.getInt("msyncErrno")!=0)
                    throw new IOException("Encrypted proxy mapping failed");
                // Deliberately do not commit here: msync must have reached the
                // encrypted backend, rather than leaving a successful RAM cache.
            }
            try(EncryptedStore reopened=EncryptedStore.open(directory,secret)) {
                byte[] data=new byte[65536];if(reopened.read("mapped-file",0,data,0,data.length)!=data.length)
                    throw new IOException("Encrypted proxy lost file size");
                for(int i=0;i<data.length;i++)if(data[i]!=(i==1?(byte)0x51:(byte)0x3d))
                    throw new IOException("Mapped write was not durably committed");
            }
            result.put("mappedWriteSurvivedBackendReopen",true);return result;
        } finally {thread.quitSafely();java.util.Arrays.fill(secret,(byte)0);}
    }
    // Feasibility test only. These transaction IDs are pinned to the host's
    // Android 16 IStorageManager.aidl; this is not a portable production API.
    private static Parcel call(IBinder binder,int transaction,int... args)throws Exception {
        Parcel data=Parcel.obtain(),reply=Parcel.obtain();
        try {
            data.writeInterfaceToken("android.os.storage.IStorageManager");
            for(int arg:args)data.writeInt(arg);
            if(!binder.transact(transaction,data,reply,0))throw new IOException("Unsupported storage transaction");
            reply.readException();return reply;
        } catch(Exception error){reply.recycle();throw error;}finally{data.recycle();}
    }
    private static ParcelFileDescriptor open(IBinder binder,int mount)throws Exception {
        Parcel reply=call(binder,75,mount,2,ParcelFileDescriptor.MODE_READ_WRITE);
        try{return reply.readInt()!=0?ParcelFileDescriptor.CREATOR.createFromParcel(reply):null;}
        finally{reply.recycle();}
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);TextView text=new TextView(this);text.setText("Testing rootless file descriptors…");setContentView(text);
        new Thread(new Runnable() {@Override public void run() {
            JSONObject report=new JSONObject();
            try {
                report.put("uid",android.os.Process.myUid()).put("sdk",Build.VERSION.SDK_INT);
                report.put("selinux",new String(java.nio.file.Files.readAllBytes(new File("/proc/self/attr/current").toPath()),"UTF-8").trim());
                for(String line:java.nio.file.Files.readAllLines(new File("/proc/self/status").toPath()))
                    if(line.startsWith("CapEff:"))report.put("capEff",line.substring(7).trim());
                java.nio.file.Path scratchRoot=java.nio.file.Files.createTempDirectory(getFilesDir().toPath(),"ce-test-");
                report.put("encryptedBackend",new JSONObject(dev.kreijstal.aosppocket.storage.EncryptedStoreTest.run(scratchRoot)));
                StorageManager storage=(StorageManager)getSystemService(STORAGE_SERVICE);
                report.put("encryptedProxy",encryptedProxy(storage,scratchRoot));
                HandlerThread callbacks=new HandlerThread("ProxyCallbacks");callbacks.start();
                byte[] scratch=new byte[65536];java.util.Arrays.fill(scratch,(byte)0x3d);
                try(ParcelFileDescriptor fd=storage.openProxyFileDescriptor(ParcelFileDescriptor.MODE_READ_WRITE,new ProxyFileDescriptorCallback() {
                    @Override public long onGetSize(){return scratch.length;}
                    @Override public int onRead(long offset,int size,byte[] data){int n=Math.min(size,scratch.length-(int)offset);if(n<=0)return 0;System.arraycopy(scratch,(int)offset,data,0,n);return n;}
                    @Override public int onWrite(long offset,int size,byte[] data){int n=Math.min(size,scratch.length-(int)offset);if(n<=0)return 0;System.arraycopy(data,0,scratch,(int)offset,n);return n;}
                    @Override public void onFsync(){}
                    @Override public void onRelease(){}
                },new Handler(callbacks.getLooper()))) {
                    report.put("publicProxy",new JSONObject(testDescriptors(fd.getFd(),-1)));
                } finally {callbacks.quitSafely();}
                Class<?> manager=Class.forName("android.os.ServiceManager");
                IBinder binder=(IBinder)manager.getMethod("getService",String.class).invoke(null,"mount");
                if(Build.VERSION.SDK_INT!=36)throw new IOException("Raw bridge probe requires the pinned Android 16 host");
                Parcel mounted=call(binder,74);
                int mountId;ParcelFileDescriptor endpoint;
                try {
                    if(mounted.readInt()==0)throw new IOException("Host did not create an AppFuse bridge");
                    mountId=mounted.readInt();endpoint=mounted.readParcelable(null);
                } finally{mounted.recycle();}
                long server=startServer(endpoint.detachFd());
                if(server==0)throw new IOException("Native AppFuse scratch server did not start");
                try {
                    try(ParcelFileDescriptor first=open(binder,mountId);
                        ParcelFileDescriptor second=open(binder,mountId)) {
                        if(first==null || second==null)throw new IOException("Host did not open the scratch proxy file");
                        report.put("cachedBridge",new JSONObject(testDescriptors(first.getFd(),second.getFd())));
                    }
                } finally {stopServer(server);}
                report.put("validation","PASS").put("filesystemReady",false)
                    .put("scope","isolated backend and feasibility tests; guest encryption is not enabled");
            } catch(Throwable error) {
                if(error instanceof InvocationTargetException && error.getCause()!=null)error=error.getCause();
                try {report.put("validation","FAIL").put("error",error.toString());}catch(Exception ignored){}
            }
            final String result=report.toString();
            try(FileOutputStream file=openFileOutput("report.json",MODE_PRIVATE)){file.write(result.getBytes("UTF-8"));}
            catch(IOException error){android.util.Log.e("PocketStorageProbe","Cannot save report",error);}
            runOnUiThread(new Runnable(){@Override public void run(){text.setText(result);}});
        }},"StorageProbe").start();
    }
}
