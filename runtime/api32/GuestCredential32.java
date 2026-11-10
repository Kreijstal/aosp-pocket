// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.content.Context;
import android.net.*;
import android.os.*;
import android.system.Os;
import java.io.*;
import java.lang.reflect.*;
import java.nio.*;
import java.util.concurrent.*;
import org.json.JSONObject;

/** System-owner credential operations. Passwords enter over stdin, never argv. */
public final class GuestCredential32 {
    private static Object service(String name,String type)throws Exception {
        IBinder binder=(IBinder)Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,name);
        if(binder==null)throw new IOException("Required guest service is unavailable");
        return Class.forName(type+"$Stub").getMethod("asInterface",IBinder.class).invoke(null,binder);
    }
    private static String line(Reader in)throws IOException {
        StringBuilder text=new StringBuilder();int c;
        while((c=in.read())!=-1 && c!='\n') {if(text.length()==512)throw new IOException("Credential exceeds bound");text.append((char)c);}
        if(c==-1 && text.length()==0)throw new EOFException("Credential input missing");
        return text.toString();
    }
    private static long control(int code)throws Exception {
        try(LocalSocket socket=new LocalSocket()) {
            socket.connect(new LocalSocketAddress("/dev/socket/pocket_ce",LocalSocketAddress.Namespace.FILESYSTEM));socket.setSoTimeout(30000);
            ByteBuffer request=ByteBuffer.allocate(88).order(ByteOrder.LITTLE_ENDIAN);
            request.putInt(0x31454350).putInt(1).putInt(3).putInt(code).putInt(1000).putInt(1000);
            socket.getOutputStream().write(request.array());DataInputStream in=new DataInputStream(socket.getInputStream());
            if(in.readUnsignedByte()!=0xa5)throw new IOException("Invalid CE control response");
            long value=Long.reverseBytes(in.readLong());
            if(Integer.reverseBytes(in.readInt())!=0 || Integer.reverseBytes(in.readInt())!=0 || value<0)throw new IOException("CE control rejected");
            return value;
        }
    }
    private static AutoCloseable credential(Class<?> type,String text)throws Exception {
        return (AutoCloseable)(text.isEmpty()?type.getMethod("createNone").invoke(null):type.getMethod("createPassword",CharSequence.class).invoke(null,text));
    }
    private static void verify(Object lock,Class<?> type,AutoCloseable credential)throws Exception {
        Class<?> callback=Class.forName("com.android.internal.widget.LockPatternUtils$CheckCredentialProgressCallback");
        boolean valid=(Boolean)lock.getClass().getMethod("checkCredential",type,int.class,callback).invoke(lock,credential,0,null);
        if(!valid)throw new SecurityException("Guest credential rejected");
    }
    private static JSONObject operate(Context context,String mode,String first,String second)throws Exception {
        Class<?> type=Class.forName("com.android.internal.widget.LockscreenCredential");
        Class<?> utility=Class.forName("com.android.internal.widget.LockPatternUtils");Object lock=utility.getConstructor(Context.class).newInstance(context);
        if(mode.equals("set")) {
            try(AutoCloseable saved=credential(type,first);AutoCloseable next=credential(type,second)) {
                if(!(Boolean)utility.getMethod("setLockCredential",type,type,int.class).invoke(lock,next,saved,0))throw new SecurityException("Guest credential rejected");
            }
            boolean secure=(Boolean)utility.getMethod("isSecure",int.class).invoke(lock,0);
            if(secure!=!second.isEmpty())throw new IOException("Credential state did not update");
            return new JSONObject().put("credentialChanged",true).put("secure",secure);
        }
        if(!mode.equals("check") && !mode.equals("prepare"))throw new IllegalArgumentException("Unknown credential operation");
        try(AutoCloseable supplied=credential(type,first)) {
            verify(lock,type,supplied);
            JSONObject result=new JSONObject().put("credentialVerified",true);
            if(mode.equals("check"))return result;
            if(control(0)!=0)throw new IOException("CE storage is already enabled");
            Object settings=service("lock_settings","com.android.internal.widget.ILockSettings");
            long handle=(Long)Class.forName("com.android.internal.widget.ILockSettings").getMethod("getLong",String.class,long.class,int.class).invoke(settings,"sp-handle",0L,0);
            if(handle==0)throw new IOException("Synthetic password is unavailable");
            Class<?> properties=Class.forName("android.os.SystemProperties"),storage=Class.forName("android.os.storage.IStorageManager");
            Method get=properties.getMethod("get",String.class,String.class),set=properties.getMethod("set",String.class,String.class);
            Object manager=service("mount",storage.getName());String previous=(String)get.invoke(null,"persist.sys.emulate_fbe","");boolean prepared=false;
            control(5);
            try {
                set.invoke(null,"persist.sys.emulate_fbe","1");
                storage.getMethod("lockUserKey",int.class).invoke(manager,0);
                if((Boolean)storage.getMethod("isUserKeyUnlocked",int.class).invoke(manager,0))throw new IOException("Storage key did not enter the locked state");
                verify(lock,type,supplied);
                long until=SystemClock.elapsedRealtime()+15000,bytes;
                while((bytes=control(6))<32) {if(SystemClock.elapsedRealtime()>=until)throw new IOException("LockSettings did not deliver its disk secret");Thread.sleep(50);}
                prepared=true;return result.put("prepared",true).put("diskSecretBytes",bytes).put("syntheticPassword",true);
            }finally {
                try{set.invoke(null,"persist.sys.emulate_fbe",previous);}
                finally{
                    control(7);
                    if(!prepared) {
                        control(8);storage.getMethod("unlockUserKey",int.class,int.class,byte[].class,byte[].class).invoke(manager,0,0,null,null);
                    }
                }
            }
        }
    }
    public static void main(String[] args) {
        try {
            if(Build.VERSION.SDK_INT!=32 || Os.getuid()!=0 || args.length!=1)throw new IllegalStateException("SDK32 owner entry required");
            Reader input=new InputStreamReader(System.in,"UTF-8");String first=line(input),second=args[0].equals("set")?line(input):null;
            Os.setgid(1000);Os.setuid(1000);Looper.prepareMainLooper();
            Class<?> activity=Class.forName("android.app.ActivityThread");Object thread=activity.getMethod("systemMain").invoke(null);
            Context context=(Context)activity.getMethod("getSystemContext").invoke(thread);
            FutureTask<JSONObject> work=new FutureTask<>(()->operate(context,args[0],first,second));
            Thread worker=new Thread(work,"PocketCredential32");worker.setDaemon(true);worker.start();
            JSONObject result=work.get(120,TimeUnit.SECONDS);System.out.println("POCKET_CREDENTIAL32_RESULT "+result);System.out.flush();System.exit(0);
        }catch(Throwable error) {
            while((error instanceof InvocationTargetException || error instanceof ExecutionException) && error.getCause()!=null)error=error.getCause();
            // Exception messages and credential objects never enter the report.
            System.err.println("POCKET_CREDENTIAL32_ERROR "+error.getClass().getSimpleName());
            StackTraceElement[] frames=error.getStackTrace();for(int i=0;i<Math.min(12,frames.length);i++)System.err.println(" at "+frames[i]);
            System.exit(error instanceof SecurityException?2:1);
        }
    }
}
