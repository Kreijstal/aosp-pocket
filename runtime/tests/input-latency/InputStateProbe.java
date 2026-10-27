// SPDX-License-Identifier: GPL-3.0-only
import android.os.IBinder;
import android.os.Parcel;
import android.view.*;
import java.io.FileDescriptor;
import java.lang.reflect.Method;

/** Guest input service diagnostics; optional physical digit tests app delivery. */
public final class InputStateProbe {
    public static void main(String[] args)throws Exception {
        android.system.Os.setgid(1000);android.system.Os.setuid(1000);
        try {
            KeyCharacterMap keys=KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD);
            System.out.println("VIRTUAL_KEY_1_UNICODE "+keys.get(KeyEvent.KEYCODE_1,0));
            System.out.println("VIRTUAL_PASSWORD_EVENTS "+(keys.getEvents(new char[]{'1','a'})!=null));
        } catch(Throwable unavailable){System.out.println("VIRTUAL_KEYMAP_ERROR "+unavailable);}
        Class<?> manager=Class.forName("android.os.ServiceManager");
        IBinder service=(IBinder)manager.getMethod("getService",String.class).invoke(null,"input_method");
        IBinder powerBinder=(IBinder)manager.getMethod("getService",String.class).invoke(null,"power");
        Object power=Class.forName("android.os.IPowerManager$Stub").getMethod("asInterface",IBinder.class).invoke(null,powerBinder);
        System.out.println("GUEST_INTERACTIVE "+Class.forName("android.os.IPowerManager").getMethod("isInteractive").invoke(power));
        try {
            Object activity=Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
            System.out.println("FOCUSED_STACK "+Class.forName("android.app.IActivityManager").getMethod("getFocusedStackInfo").invoke(activity));
        } catch(Exception unavailable){System.out.println("FOCUS_QUERY_FAILED "+unavailable);}
        Parcel data=Parcel.obtain(),reply=Parcel.obtain();
        try {
            data.writeFileDescriptor(FileDescriptor.out);data.writeStringArray(new String[0]);
            service.transact(IBinder.DUMP_TRANSACTION,data,reply,0);
        } finally {data.recycle();reply.recycle();}
        try(android.net.LocalSocket socket=new android.net.LocalSocket()) {
            socket.connect(new android.net.LocalSocketAddress("pocket-numeric-typing-test",android.net.LocalSocketAddress.Namespace.ABSTRACT));
            socket.setSoTimeout(3000);java.io.DataOutputStream out=new java.io.DataOutputStream(socket.getOutputStream());
            out.writeInt(0);out.flush();System.out.println("TYPING_APP_STATE "+new java.io.DataInputStream(socket.getInputStream()).readUTF());
        } catch(java.io.IOException unavailable){System.out.println("NO_TYPING_SNAPSHOT "+unavailable);}
        if(args.length>0 && (args[0].equals("--physical-digit") || args[0].equals("--wake"))) {
            IBinder binder=(IBinder)manager.getMethod("getService",String.class).invoke(null,"input");
            Object input=Class.forName("android.hardware.input.IInputManager$Stub").getMethod("asInterface",IBinder.class).invoke(null,binder);
            Method inject=Class.forName("android.hardware.input.IInputManager").getMethod("injectInputEvent",InputEvent.class,int.class);
            long now=android.os.SystemClock.uptimeMillis();
            for(int action:new int[]{KeyEvent.ACTION_DOWN,KeyEvent.ACTION_UP}) {
                KeyEvent event=new KeyEvent(now,android.os.SystemClock.uptimeMillis(),action,args[0].equals("--wake")?KeyEvent.KEYCODE_WAKEUP:KeyEvent.KEYCODE_1,0);
                event.setSource(InputDevice.SOURCE_KEYBOARD);System.out.println("PHYSICAL_ACCEPTED "+inject.invoke(input,event,2));
            }
            System.out.println("PHYSICAL_DIGIT_SUBMITTED");
        }
        System.exit(0);
    }
}
