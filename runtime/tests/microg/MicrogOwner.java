import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.os.Looper;
import android.os.Binder;
import android.os.Parcel;
import android.os.IBinder;
import android.os.RemoteException;
import android.content.IntentSender;
import android.content.pm.PackageInstaller;
import java.io.File;
import java.io.FileInputStream;
import java.io.OutputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Owner-only diagnostics: normal PackageInstaller and the real microG UI. */
public final class MicrogOwner {
    private static void install(Context context, File apk) throws Exception {
        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(context.getPackageManager().getPackageArchiveInfo(apk.getPath(),0).packageName);params.setSize(apk.length());
        // The owner console uses logical root; request replacement explicitly
        // instead of uninstalling the test app and losing its private data.
        Field flags = params.getClass().getDeclaredField("installFlags");flags.setAccessible(true);
        flags.setInt(params, flags.getInt(params) | 2);
        CountDownLatch done = new CountDownLatch(1);final Intent[] result = new Intent[1];
        Binder receiver = new Binder() {
            @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                if (code != IBinder.FIRST_CALL_TRANSACTION) return super.onTransact(code, data, reply, flags);
                data.enforceInterface("android.content.IIntentSender");data.readInt();
                result[0] = data.readInt() == 0 ? null : Intent.CREATOR.createFromParcel(data);
                done.countDown();return true;
            }
        };
        Class<?> sender = Class.forName("android.content.IIntentSender");
        Object target = Class.forName("android.content.IIntentSender$Stub").getMethod("asInterface", IBinder.class).invoke(null, receiver);
        Constructor<IntentSender> ctor = IntentSender.class.getDeclaredConstructor(sender);ctor.setAccessible(true);
        int id = installer.createSession(params);
        try (PackageInstaller.Session session = installer.openSession(id)) {
            try (FileInputStream in = new FileInputStream(apk); OutputStream out = session.openWrite("base.apk", 0, apk.length())) {
                byte[] buffer = new byte[65536];int count;
                while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
                session.fsync(out);
            }
            session.commit(ctor.newInstance(target));
        }
        if (!done.await(90, TimeUnit.SECONDS) || result[0] == null) throw new IllegalStateException("Installer callback missing");
        if (result[0].getIntExtra(PackageInstaller.EXTRA_STATUS, -1) != PackageInstaller.STATUS_SUCCESS)
            throw new IllegalStateException(result[0].getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
        System.out.println("RUNTIME_PROBE_INSTALLED");
    }
    public static void main(String[] args) {
        try {
            if(!args[0].equals("--install")){android.system.Os.setgid(1000);android.system.Os.setuid(1000);}
            Looper.prepareMainLooper();
            Class<?> thread = Class.forName("android.app.ActivityThread");
            Object main = thread.getMethod("systemMain").invoke(null);
            Context context = (Context)thread.getMethod("getSystemContext").invoke(main);
            if(args[0].equals("--install"))install(context,new File(args[1]));
            else if(args[0].equals("--force-stop")) {
                Object manager=Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
                Class.forName("android.app.IActivityManager").getMethod("forceStopPackage",String.class,int.class)
                        .invoke(manager,args[1],0);
                System.out.println("DIAGNOSTIC_APP_STOPPED");
            }
            else if(args[0].equals("--contacts-permission")) {
                System.out.println("CONTACTS_TEST_PERMISSION="+context.getPackageManager().checkPermission("android.permission.READ_CONTACTS","dev.kreijstal.pocketmicrogtest"));
            }
            else if(args[0].equals("--grant-contacts")||args[0].equals("--revoke-contacts")) {
                Object binder=Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,"package");
                Object pm=Class.forName("android.content.pm.IPackageManager$Stub").getMethod("asInterface",IBinder.class).invoke(null,binder);
                String method=args[0].equals("--grant-contacts")?"grantRuntimePermission":"revokeRuntimePermission";
                Class.forName("android.content.pm.IPackageManager").getMethod(method,String.class,String.class,int.class).invoke(pm,"dev.kreijstal.pocketmicrogtest","android.permission.READ_CONTACTS",0);
                System.out.println("CONTACTS_TEST_PERMISSION_UPDATED");
            }
            else if(args[0].equals("--selfcheck")) {
                final Throwable[] error={null};
                Thread worker=new Thread(()->{try{selfcheck(context);}catch(Throwable failure){error[0]=failure;}},"MicrogUiInspector");
                worker.start();worker.join(210000);
                if(worker.isAlive())throw new IllegalStateException("Self-Check UI inspection timed out");
                if(error[0]!=null)throw error[0];
            }
            else {
                Intent intent=new Intent().setComponent(ComponentName.unflattenFromString(args[1])).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                Method launch=Class.forName("dev.kreijstal.aosppocket.guest.FrameworkProbe").getDeclaredMethod("launch",Intent.class);
                launch.setAccessible(true);launch.invoke(null,intent);
                System.out.println("MICROG_ACTIVITY_STARTED");
            }
            System.exit(0);
        } catch (Throwable failure) { failure.printStackTrace(); System.exit(1); }
    }
    private static void selfcheck(Context context)throws Exception {
        android.os.PowerManager power=(android.os.PowerManager)context.getSystemService(Context.POWER_SERVICE);
        android.os.PowerManager.WakeLock awake=power.newWakeLock(android.os.PowerManager.SCREEN_BRIGHT_WAKE_LOCK|android.os.PowerManager.ACQUIRE_CAUSES_WAKEUP,"Pocket:MicrogSelfCheck");
        awake.acquire(240000);
        Intent intent=new Intent().setComponent(new ComponentName("com.google.android.gms","org.microg.gms.ui.SelfCheckFragment$AsActivity")).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        Method launch=Class.forName("dev.kreijstal.aosppocket.guest.FrameworkProbe").getDeclaredMethod("launch",Intent.class);
        launch.setAccessible(true);launch.invoke(null,intent);
        Class<?> connection=Class.forName("android.app.IUiAutomationConnection");
        Object owner=Class.forName("android.app.UiAutomationConnection").getConstructor().newInstance();
        android.app.UiAutomation automation=(android.app.UiAutomation)android.app.UiAutomation.class.getConstructor(Looper.class,connection).newInstance(Looper.getMainLooper(),owner);
        android.app.UiAutomation.class.getMethod("connect",int.class).invoke(automation,1);
        try {
            android.accessibilityservice.AccessibilityServiceInfo info=automation.getServiceInfo();
            info.flags|=android.accessibilityservice.AccessibilityServiceInfo.FLAG_REPORT_VIEW_IDS
                    |android.accessibilityservice.AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS
                    |android.accessibilityservice.AccessibilityServiceInfo.FLAG_INCLUDE_NOT_IMPORTANT_VIEWS;
            automation.setServiceInfo(info);
            long deadline=android.os.SystemClock.uptimeMillis()+180000;
            boolean ready=false;
            while(android.os.SystemClock.uptimeMillis()<deadline) {
                android.view.accessibility.AccessibilityNodeInfo root=googleRoot(automation);
                if(root!=null)try{ready=node(root).toString().contains("\"checkable\":true");}finally{root.recycle();}
                if(ready)break;Thread.sleep(500);
            }
            if(!ready)throw new IllegalStateException("microG Self-Check did not expose its checkbox content");
            // An existing settings task may retain its previous scroll position.
            for(int i=0;i<12;i++) {
                android.view.accessibility.AccessibilityNodeInfo root=googleRoot(automation);
                if(root==null)break;
                boolean moved;try{moved=scroll(root,android.view.accessibility.AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD);}finally{root.recycle();}
                if(!moved)break;Thread.sleep(200);
            }
            org.json.JSONArray pages=new org.json.JSONArray();
            for(int page=0;page<8;page++) {
                Thread.sleep(1500);android.view.accessibility.AccessibilityNodeInfo root=googleRoot(automation);
                if(root==null)continue;
                try {
                    pages.put(node(root));
                    if(!scroll(root,android.view.accessibility.AccessibilityNodeInfo.ACTION_SCROLL_FORWARD))break;
                }finally{root.recycle();}
            }
            System.out.println("MICROG_SELFCHECK_UI "+pages);
        }finally{android.app.UiAutomation.class.getMethod("disconnect").invoke(automation);if(awake.isHeld())awake.release();}
    }
    private static android.view.accessibility.AccessibilityNodeInfo googleRoot(android.app.UiAutomation automation) {
        android.view.accessibility.AccessibilityNodeInfo root=automation.getRootInActiveWindow();
        if(root!=null && "com.google.android.gms".contentEquals(root.getPackageName()==null?"":root.getPackageName()))return root;
        if(root!=null)root.recycle();root=null;
        for(android.view.accessibility.AccessibilityWindowInfo window:automation.getWindows()) {
            android.view.accessibility.AccessibilityNodeInfo candidate=window.getRoot();
            if(candidate!=null && "com.google.android.gms".contentEquals(candidate.getPackageName()==null?"":candidate.getPackageName())){if(root!=null)root.recycle();root=candidate;}
            else if(candidate!=null)candidate.recycle();
            window.recycle();
        }
        return root;
    }
    private static org.json.JSONObject node(android.view.accessibility.AccessibilityNodeInfo node)throws Exception {
        org.json.JSONObject result=new org.json.JSONObject();
        result.put("package",node.getPackageName());result.put("text",node.isPassword()?"<password field>":node.getText());result.put("description",node.isPassword()?null:node.getContentDescription());
        result.put("id",node.getViewIdResourceName());result.put("checkable",node.isCheckable());result.put("checked",node.isChecked());
        org.json.JSONArray children=new org.json.JSONArray();
        for(int i=0;i<node.getChildCount();i++) {
            android.view.accessibility.AccessibilityNodeInfo child=node.getChild(i);
            if(child!=null)try{children.put(node(child));}finally{child.recycle();}
        }
        if(children.length()>0)result.put("children",children);return result;
    }
    private static boolean scroll(android.view.accessibility.AccessibilityNodeInfo node,int action) {
        if(node.isScrollable() && node.performAction(action))return true;
        for(int i=0;i<node.getChildCount();i++) {
            android.view.accessibility.AccessibilityNodeInfo child=node.getChild(i);
            if(child!=null)try{if(scroll(child,action))return true;}finally{child.recycle();}
        }
        return false;
    }
}
