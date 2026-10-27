// SPDX-License-Identifier: GPL-3.0-only
import android.content.Context;
import android.net.*;
import android.os.*;
import android.system.Os;
import java.io.*;
import java.lang.reflect.*;
import java.nio.*;
import org.json.JSONObject;

/** Owner migration helper. Credentials stay in Android LockSettings. */
public final class GuestCeKey {
    static String credential;
    static Object service(String name,String type)throws Exception {
        IBinder binder=(IBinder)Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,name);
        return Class.forName(type+"$Stub").getMethod("asInterface",IBinder.class).invoke(null,binder);
    }
    static long control(int code)throws Exception {
        try(LocalSocket socket=new LocalSocket()) {
            socket.connect(new LocalSocketAddress("/dev/socket/pocket_ce",LocalSocketAddress.Namespace.FILESYSTEM));
            ByteBuffer request=ByteBuffer.allocate(88).order(ByteOrder.LITTLE_ENDIAN);
            request.putInt(0x31454350).putInt(1).putInt(3).putInt(code).putInt(1000).putInt(1000);
            socket.getOutputStream().write(request.array());DataInputStream in=new DataInputStream(socket.getInputStream());
            if(in.readUnsignedByte()!=0xa5)throw new IOException("CE protocol mismatch");long result=Long.reverseBytes(in.readLong());
            if(result<0)throw new IOException("CE operation rejected: "+result);return result;
        }
    }
    public static void main(String[] args) {
        try {
            credential=new BufferedReader(new InputStreamReader(System.in,"UTF-8")).readLine();
            if(credential==null || credential.isEmpty())throw new SecurityException("Guest credential required");
            Os.setgid(1000);Os.setuid(1000);Looper.prepareMainLooper();
            Class<?> activity=Class.forName("android.app.ActivityThread");Object thread=activity.getMethod("systemMain").invoke(null);
            Context context=(Context)activity.getMethod("getSystemContext").invoke(thread);
            final Throwable[] failure={null};Thread worker=new Thread(()->{try{prepare(context,args.length>0?args[0]:"inspect");}catch(Throwable error){failure[0]=error;}},"GuestCeCredential");
            worker.start();worker.join(120000);if(worker.isAlive())throw new IOException("Guest credential operation timed out");
            if(failure[0]!=null)throw failure[0];System.exit(0);
        }catch(Throwable error) {
            if(error instanceof InvocationTargetException && error.getCause()!=null)error=error.getCause();
            String message=String.valueOf(error);if(credential!=null)message=message.replace(credential,"<redacted>");
            System.err.println("GUEST_CE_KEY_FAILED "+message);System.exit(1);
        }
    }
    static void prepare(Context context,String mode)throws Exception {
        Class<?> lpu=Class.forName("com.android.internal.widget.LockPatternUtils");Object lock=lpu.getConstructor(Context.class).newInstance(context);
        Method verify=lpu.getMethod("checkPassword",String.class,int.class);
        if(!(Boolean)verify.invoke(lock,credential,0))throw new SecurityException("Guest credential rejected");
        Class<?> settings=Class.forName("com.android.internal.widget.ILockSettings");Object locks=service("lock_settings",settings.getName());
        long handle=(Long)settings.getMethod("getLong",String.class,long.class,int.class).invoke(locks,"sp-handle",0L,0);
        if(handle==0)throw new SecurityException("Synthetic-password migration is required first");
        int bytes=0;
        if(mode.equals("prepare")) {
            if(control(0)!=0)throw new IOException("Guest encryption is already enabled");
            Class<?> props=Class.forName("android.os.SystemProperties"),storage=Class.forName("android.os.storage.IStorageManager");
            Method get=props.getMethod("get",String.class,String.class),set=props.getMethod("set",String.class,String.class);
            String original=(String)get.invoke(null,"persist.sys.emulate_fbe","");Object manager=service("mount",storage.getName());
            boolean prepared=false;control(5);
            try {
                // Temporarily enable Android's existing emulation gate so a
                // fresh authenticated unlock delivers the real SP disk secret
                // to vold. ro.crypto.state/type are never changed here, and no
                // storage encryption is claimed during this preparation phase.
                set.invoke(null,"persist.sys.emulate_fbe","1");
                storage.getMethod("lockUserKey",int.class).invoke(manager,0);
                if(!(Boolean)verify.invoke(lock,credential,0))throw new SecurityException("Guest credential recheck failed");
                bytes=(int)control(6);if(bytes<32)throw new IOException("Android did not deliver its synthetic disk secret");prepared=true;
            } finally {
                set.invoke(null,"persist.sys.emulate_fbe",original);control(7);
                if(!prepared) {
                    control(8);
                    storage.getMethod("unlockUserKey",int.class,int.class,byte[].class,byte[].class).invoke(manager,0,0,null,null);
                }
            }
        }
        JSONObject result=new JSONObject().put("validation","PASS").put("credentialVerified",true).put("syntheticPassword",true).put("prepared",mode.equals("prepare"));
        if(bytes>0)result.put("diskSecretBytes",bytes).put("keySource","Android LockSettings via vold");
        System.out.println("GUEST_CE_KEY_RESULT "+result);
    }
}
