// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.app.ActivityManager;
import android.content.*;
import android.content.pm.PackageInstaller;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.*;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.MotionEvent;
import java.io.*;
import java.lang.reflect.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.regex.*;

/** Runs inside the guest. Uses its real package, activity and input services. */
public final class FrameworkProbe {
    private static final String PACKAGE="dev.kreijstal.pocketguesttest";
    private static void check(boolean condition,String message) throws IOException {
        if(!condition)throw new IOException(message);
    }
    static Object service(String name,String stub) throws Exception {
        IBinder binder=(IBinder)Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,name);
        check(binder!=null,"Missing service: "+name);
        return Class.forName(stub).getMethod("asInterface",IBinder.class).invoke(null,binder);
    }
    static Context context() throws Exception {
        if(Looper.myLooper()==null)Looper.prepare();
        Class<?> thread=Class.forName("android.app.ActivityThread");
        Object main=thread.getMethod("systemMain").invoke(null);
        return (Context)thread.getMethod("getSystemContext").invoke(main);
    }
    private static void capture(String filename) throws Exception {
        ProcessBuilder command=new ProcessBuilder("/system/bin/screencap","-p",filename);
        command.redirectErrorStream(true);command.redirectOutput(new File(filename+".log"));
        java.lang.Process process=command.start();
        check(process.waitFor(20,TimeUnit.SECONDS) && process.exitValue()==0,"screencap failed");
        Bitmap bitmap=BitmapFactory.decodeFile(filename);
        check(bitmap!=null && bitmap.getWidth()>0 && bitmap.getHeight()>0,"Invalid compositor image");
        Set<Integer> colors=new HashSet<>();
        for(int y=0;y<bitmap.getHeight();y+=11)for(int x=0;x<bitmap.getWidth();x+=11)colors.add(bitmap.getPixel(x,y));
        check(colors.size()>8,"Compositor image lacks visible UI detail");
        System.out.println("DIRECT_FRAMEWORK_FRAME path="+filename+" width="+bitmap.getWidth()+" height="+bitmap.getHeight()+" sampled_colors="+colors.size());
        bitmap.recycle();
    }
    private static void launcher(Context context) throws Exception {
        ActivityManager manager=(ActivityManager)context.getSystemService(Context.ACTIVITY_SERVICE);
        boolean home=false;
        for(int attempt=0;attempt<250;++attempt) {
            List<ActivityManager.RunningTaskInfo> tasks=manager.getRunningTasks(1);
            if(!tasks.isEmpty() && tasks.get(0).topActivity!=null &&
               tasks.get(0).topActivity.getPackageName().equals("com.android.launcher3") &&
               log().contains("Displayed com.android.launcher3/")) {home=true;break;}
            Thread.sleep(200);
        }
        check(home,"Launcher did not reach foreground and complete its first draw");
        capture("/data/pocket-launcher.png");
        System.out.println("DIRECT_FRAMEWORK_LAUNCHER_PASS foreground=com.android.launcher3 compositor=verified");
    }
    static void install(Context context,File[] apks,String packageName) throws Exception {
        PackageInstaller installer=context.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params=new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        long size=0;for(File apk:apks)size+=apk.length();
        params.setAppPackageName(packageName);params.setSize(size);
        CountDownLatch delivered=new CountDownLatch(1);final Intent[] result=new Intent[1];
        Binder receiver=new Binder() {
            @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags) throws RemoteException {
                if(code!=IBinder.FIRST_CALL_TRANSACTION)return super.onTransact(code,data,reply,flags);
                data.enforceInterface("android.content.IIntentSender");data.readInt();
                result[0]=data.readInt()!=0?Intent.CREATOR.createFromParcel(data):null;
                delivered.countDown();return true;
            }
        };
        Object target=Class.forName("android.content.IIntentSender$Stub").getMethod("asInterface",IBinder.class).invoke(null,receiver);
        Constructor<IntentSender> constructor=IntentSender.class.getDeclaredConstructor(Class.forName("android.content.IIntentSender"));
        constructor.setAccessible(true);IntentSender callback=constructor.newInstance(target);
        int sessionId=installer.createSession(params);
        try(PackageInstaller.Session session=installer.openSession(sessionId)) {
            for(File apk:apks) {
                try(InputStream input=new FileInputStream(apk);OutputStream output=session.openWrite(apk.getName(),0,apk.length())) {
                    byte[] bytes=new byte[32768];int count;
                    while((count=input.read(bytes))>=0)output.write(bytes,0,count);
                    session.fsync(output);
                }
            }
            session.commit(callback);
        }
        check(delivered.await(60,TimeUnit.SECONDS),"PackageInstaller callback timed out");
        check(result[0]!=null,"PackageInstaller returned no intent");
        int status=result[0].getIntExtra(PackageInstaller.EXTRA_STATUS,Integer.MIN_VALUE);
        check(status==PackageInstaller.STATUS_SUCCESS,"PackageInstaller status="+status+" message="+result[0].getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
        check(context.getPackageManager().getPackageInfo(packageName,0)!=null,"Installed package is missing");
        System.out.println("DIRECT_FRAMEWORK_INSTALL_PASS package="+packageName+" session="+sessionId);
    }
    static void launch(Intent intent) throws Exception {
        Object manager=Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        Method start=null;
        for(Method candidate:Class.forName("android.app.IActivityManager").getMethods())
            if(candidate.getName().equals("startActivityAsUser") && candidate.getParameterCount()==11)start=candidate;
        check(start!=null,"Unsupported activity service ABI");
        int status=(Integer)start.invoke(manager,null,"android",intent,null,null,null,0,0,null,null,0);
        check(status>=0,"Activity start failed: "+status);
    }
    private static String log() throws IOException {
        return new String(Files.readAllBytes(Paths.get("/pocket-console")),"UTF-8");
    }
    private static void tap() throws Exception {
        Matcher bounds=null;
        for(int attempt=0;attempt<150;++attempt) {
            Matcher found=Pattern.compile("POCKET_GUEST_BUTTON center=([0-9]+),([0-9]+)").matcher(log());
            while(found.find())bounds=Pattern.compile("([0-9]+),([0-9]+)").matcher(found.group(1)+","+found.group(2));
            if(bounds!=null && bounds.find())break;
            bounds=null;Thread.sleep(200);
        }
        check(bounds!=null,"Test button layout was not reported");
        int x=Integer.parseInt(bounds.group(1)),y=Integer.parseInt(bounds.group(2));
        Object input=service("input","android.hardware.input.IInputManager$Stub");
        Method inject=Class.forName("android.hardware.input.IInputManager").getMethod("injectInputEvent",InputEvent.class,int.class);
        long down=SystemClock.uptimeMillis();
        for(int action:new int[]{MotionEvent.ACTION_DOWN,MotionEvent.ACTION_UP}) {
            MotionEvent event=MotionEvent.obtain(down,SystemClock.uptimeMillis(),action,x,y,0);event.setSource(InputDevice.SOURCE_TOUCHSCREEN);
            try {check((Boolean)inject.invoke(input,event,2),"Input injection was rejected");}finally {event.recycle();}
            SystemClock.sleep(40);
        }
        boolean clicked=false;
        for(int attempt=0;attempt<100;++attempt) {
            if(log().contains("tag=PocketGuestTest tap=1")) {clicked=true;break;}
            Thread.sleep(100);
        }
        check(clicked,"Guest button did not receive the tap");
        capture("/data/pocket-interaction.png");
        System.out.println("DIRECT_FRAMEWORK_INPUT_PASS tap=1 coordinates="+x+","+y);
    }
    public static void main(String[] args) {
        try {
            check(args.length==1 && android.os.Process.myUid()==1000,"Expected system role and one APK path");
            check(android.graphics.Typeface.DEFAULT!=null,"Default typeface missing");
            Context context=context();launcher(context);install(context,new File[]{new File(args[0])},PACKAGE);launch(new Intent(Intent.ACTION_MAIN).setComponent(new ComponentName(PACKAGE,PACKAGE+".MainActivity")));tap();
            System.out.println("DIRECT_FRAMEWORK_SMOKE_PASS launcher=1 install=1 activity=1 input=1");
        } catch(Throwable error) {
            System.err.println("DIRECT_FRAMEWORK_SMOKE_FAIL "+error);error.printStackTrace();System.exit(1);
        }
    }
}
