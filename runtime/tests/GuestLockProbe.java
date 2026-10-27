// SPDX-License-Identifier: GPL-3.0-only
import android.app.KeyguardManager;
import android.app.UiAutomation;
import android.accessibilityservice.AccessibilityServiceInfo;
import android.content.Context;
import android.os.Bundle;
import android.os.Looper;
import android.system.Os;
import android.util.DisplayMetrics;
import android.view.*;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityWindowInfo;
import java.io.*;
import java.lang.reflect.*;
import org.json.JSONObject;

/** Owner-run guest lock test. Password arrives on stdin and is never logged. */
public final class GuestLockProbe {
    static String password;
    static void require(boolean ok,String reason) {
        if(!ok)throw new AssertionError(reason);
    }
    static boolean fillPassword(AccessibilityNodeInfo node) {
        if(node==null)return false;
        if(node.isEditable() && node.isPassword() && "com.android.systemui".contentEquals(node.getPackageName())) {
            node.performAction(AccessibilityNodeInfo.ACTION_FOCUS);
            Bundle value=new Bundle();
            value.putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE,password);
            return node.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT,value);
        }
        for(int i=0;i<node.getChildCount();i++) {
            AccessibilityNodeInfo child=node.getChild(i);
            try {if(fillPassword(child))return true;}
            finally {if(child!=null)child.recycle();}
        }
        return false;
    }
    static void fillPasswordThroughUiAutomation() throws Exception {
        Class<?> connection=Class.forName("android.app.IUiAutomationConnection");
        Object owner=Class.forName("android.app.UiAutomationConnection").getConstructor().newInstance();
        UiAutomation automation=(UiAutomation)UiAutomation.class.getConstructor(Looper.class,connection)
                .newInstance(Looper.getMainLooper(),owner);
        UiAutomation.class.getMethod("connect",int.class).invoke(automation,1);
        try {
            AccessibilityServiceInfo info=automation.getServiceInfo();
            info.flags|=AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS
                    |AccessibilityServiceInfo.FLAG_REPORT_VIEW_IDS
                    |AccessibilityServiceInfo.FLAG_INCLUDE_NOT_IMPORTANT_VIEWS;
            automation.setServiceInfo(info);
            long deadline=android.os.SystemClock.uptimeMillis()+10000;boolean filled=false;
            while(!filled && android.os.SystemClock.uptimeMillis()<deadline) {
                AccessibilityNodeInfo root=automation.getRootInActiveWindow();
                try {filled=fillPassword(root);}
                finally {if(root!=null)root.recycle();}
                if(!filled)for(AccessibilityWindowInfo window:automation.getWindows()) {
                    root=window.getRoot();
                    try {if(fillPassword(root)){filled=true;break;}}
                    finally {if(root!=null)root.recycle();window.recycle();}
                }
                if(!filled)Thread.sleep(300);
            }
            require(filled,"Guest password field not accessible for text entry");
        } finally {UiAutomation.class.getMethod("disconnect").invoke(automation);}
    }
    static void unlock(Context context,KeyguardManager keyguard) throws Exception {
        Class<?> type=Class.forName("android.hardware.input.InputManager");
        Object input=type.getMethod("getInstance").invoke(null);
        Method inject=type.getMethod("injectInputEvent",InputEvent.class,int.class);
        for(int action:new int[]{KeyEvent.ACTION_DOWN,KeyEvent.ACTION_UP}) {
            KeyEvent event=new KeyEvent(android.os.SystemClock.uptimeMillis(),android.os.SystemClock.uptimeMillis(),
                    action,KeyEvent.KEYCODE_WAKEUP,0);
            event.setSource(InputDevice.SOURCE_KEYBOARD);
            require((Boolean)inject.invoke(input,event,2),"Guest wake injection failed");
        }
        Thread.sleep(1500);
        DisplayMetrics size=new DisplayMetrics();
        ((WindowManager)context.getSystemService(Context.WINDOW_SERVICE)).getDefaultDisplay().getRealMetrics(size);
        long down=android.os.SystemClock.uptimeMillis();
        for(int i=0;i<=10;i++) {
            int action=i==0?MotionEvent.ACTION_DOWN:i==10?MotionEvent.ACTION_UP:MotionEvent.ACTION_MOVE;
            MotionEvent event=MotionEvent.obtain(down,android.os.SystemClock.uptimeMillis(),action,
                    size.widthPixels*0.5f,size.heightPixels*(0.85f-0.06f*i),0);
            event.setSource(InputDevice.SOURCE_TOUCHSCREEN);
            try {require((Boolean)inject.invoke(input,event,2),"Guest swipe injection failed");}
            finally {event.recycle();}
            Thread.sleep(40);
        }
        Thread.sleep(1500);
        // Request the real secure bouncer explicitly. This avoids depending on
        // the lock-screen swipe animation while retaining UI password entry.
        Class<?> wmApi=Class.forName("android.view.IWindowManager");
        Object wmBinder=Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,"window");
        Object wm=Class.forName("android.view.IWindowManager$Stub").getMethod("asInterface",android.os.IBinder.class).invoke(null,wmBinder);
        wmApi.getMethod("dismissKeyguard",Class.forName("com.android.internal.policy.IKeyguardDismissCallback"),CharSequence.class).invoke(wm,null,null);
        Thread.sleep(1500);
        KeyEvent[] keys=KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD).getEvents(password.toCharArray());
        if(keys==null)fillPasswordThroughUiAutomation();
        else {
            boolean accepted=true;
            for(KeyEvent event:keys) {
                event.setSource(InputDevice.SOURCE_KEYBOARD);
                if(!(Boolean)inject.invoke(input,event,2)){accepted=false;break;}
            }
            // A secure bouncer may reject a generated hardware-key sequence.
            // Replace the field through the same normal UI accessibility path
            // used when a character has no key mapping, then submit normally.
            if(!accepted)fillPasswordThroughUiAutomation();
        }
        for(int action:new int[]{KeyEvent.ACTION_DOWN,KeyEvent.ACTION_UP}) {
            KeyEvent event=new KeyEvent(android.os.SystemClock.uptimeMillis(),android.os.SystemClock.uptimeMillis(),
                    action,KeyEvent.KEYCODE_ENTER,0);
            require((Boolean)inject.invoke(input,event,2),"Guest enter injection failed");
        }
        long deadline=android.os.SystemClock.uptimeMillis()+10000;
        while(keyguard.isKeyguardLocked() && android.os.SystemClock.uptimeMillis()<deadline)Thread.sleep(200);
        require(!keyguard.isKeyguardLocked(),"Guest UI stayed locked after password entry");
    }
    public static void main(String[] args) {
        try {
            boolean status=args.length>0 && "status".equals(args[0]);
            password=status?null:new BufferedReader(new InputStreamReader(System.in,"UTF-8")).readLine();
            require(status || (password!=null && password.length()>=4),"Expected guest password on stdin");
            Os.setgid(1000);Os.setuid(1000);
            require(android.os.Process.myUid()==1000,"Guest system identity was not established");
            Looper.prepareMainLooper();
            Class<?> thread=Class.forName("android.app.ActivityThread");
            Object main=thread.getMethod("systemMain").invoke(null);
            Context context=(Context)thread.getMethod("getSystemContext").invoke(main);
            final Throwable[] failure={null};
            Thread worker=new Thread(new Runnable() {
                @Override public void run() {
                    try {test(context,args.length>0?args[0]:"verify");}
                    catch(Throwable error){failure[0]=error;}
                }
            },"GuestLockTest");
            worker.start();worker.join(120000);
            require(!worker.isAlive(),"Guest credential operation timed out");
            if(failure[0]!=null)throw failure[0];
            System.exit(0);
        } catch(Throwable error) {
            if(error instanceof InvocationTargetException && error.getCause()!=null)error=error.getCause();
            System.err.println("GUEST_LOCK_FAILED "+String.valueOf(error).replace(password==null?"\u0000":password,"<redacted>"));
            System.exit(1);
        }
    }
    static void test(Context context,String mode) throws Exception {
        Class<?> type=Class.forName("com.android.internal.widget.LockPatternUtils");
        Object lock=type.getConstructor(Context.class).newInstance(context);
        Method check=type.getMethod("checkPassword",String.class,int.class);
        KeyguardManager keyguard=(KeyguardManager)context.getSystemService(Context.KEYGUARD_SERVICE);
        boolean before=keyguard.isDeviceSecure();
        if("status".equals(mode)) {
            System.out.println("GUEST_LOCK_RESULT "+new JSONObject().put("validation","PASS")
                    .put("mode",mode).put("uid",android.os.Process.myUid()).put("deviceSecure",before)
                    .put("keyguardSecure",keyguard.isKeyguardSecure()).put("keyguardLocked",keyguard.isKeyguardLocked()));
            return;
        }
        if("setup".equals(mode) && !before) {
            type.getMethod("saveLockPassword",String.class,String.class,int.class,int.class)
                    .invoke(lock,password,null,0x40000,0);
        }
        require(keyguard.isDeviceSecure(),"Guest still has no secure credential");
        require((Boolean)check.invoke(lock,password,0),"Correct credential rejected");
        if("setup".equals(mode)) {
            type.getMethod("setLockScreenDisabled",boolean.class,int.class).invoke(lock,false,0);
            require(!(Boolean)check.invoke(lock,password+"x",0),"Wrong password was not rejected");
            require((Boolean)check.invoke(lock,password,0),"Correct credential rejected after wrong attempt");
        }
        require(!(Boolean)type.getMethod("isLockScreenDisabled",int.class).invoke(lock,0),"Guest lock screen is disabled");
        boolean lockedBefore=keyguard.isKeyguardLocked();
        if("unlock".equals(mode)) {
            require(lockedBefore,"Expected the guest keyguard after restart");
            android.os.PowerManager power=(android.os.PowerManager)context.getSystemService(Context.POWER_SERVICE);
            android.os.PowerManager.WakeLock awake=power.newWakeLock(android.os.PowerManager.SCREEN_BRIGHT_WAKE_LOCK
                    |android.os.PowerManager.ACQUIRE_CAUSES_WAKEUP,"Pocket:GuestLockTest");
            awake.acquire(45000);
            try {unlock(context,keyguard);}
            finally {if(awake.isHeld())awake.release();}
        }
        JSONObject report=new JSONObject().put("validation","PASS").put("mode",mode)
                .put("uid",android.os.Process.myUid()).put("previouslySecure",before)
                .put("deviceSecure",keyguard.isDeviceSecure()).put("keyguardSecure",keyguard.isKeyguardSecure())
                .put("keyguardLocked",keyguard.isKeyguardLocked()).put("correctPasswordVerified",true)
                .put("interactive",((android.os.PowerManager)context.getSystemService(Context.POWER_SERVICE)).isInteractive())
                .put("lockScreenEnabled",true);
        if("setup".equals(mode))report.put("wrongPasswordRejected",true);
        if("unlock".equals(mode))report.put("uiPasswordUnlockVerified",true).put("previouslyLocked",lockedBefore);
        System.out.println("GUEST_LOCK_RESULT "+report);
    }
}
