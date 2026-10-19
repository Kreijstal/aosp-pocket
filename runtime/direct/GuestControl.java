// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.content.*;
import android.content.pm.PackageInfo;
import android.graphics.Bitmap;
import android.net.*;
import android.os.*;
import android.view.*;
import java.io.*;
import java.lang.reflect.*;
import java.util.concurrent.Semaphore;

/** App-private host connection to the real Android 9 compositor/input services. */
public final class GuestControl {
    private static final int WIDTH=480, HEIGHT=800;
    private static final Semaphore clients=new Semaphore(4);
    private static Object input;
    private static Method inject;
    private static Context context;
    private static boolean inject(InputEvent event) throws Exception {
        return (Boolean)inject.invoke(input,event,2);
    }
    private static void serve(LocalSocket socket) {
        try(LocalSocket peer=socket) {
            // All guest processes share one host UID; this is an app-private
            // control channel, not a security boundary between guest apps.
            peer.setSoTimeout(0);
            DataInputStream in=new DataInputStream(new BufferedInputStream(peer.getInputStream()));
            DataOutputStream out=new DataOutputStream(new BufferedOutputStream(peer.getOutputStream()));
            out.writeInt(0x504b5439);out.writeInt(WIDTH);out.writeInt(HEIGHT);out.flush();
            while(true) {
                int op=in.readInt();
                if(op==1) {
                    Bitmap bitmap=(Bitmap)Class.forName("android.view.SurfaceControl")
                        .getMethod("screenshot",android.graphics.Rect.class,int.class,int.class,int.class)
                        .invoke(null,new android.graphics.Rect(),WIDTH,HEIGHT,0);
                    if(bitmap==null)throw new IOException("Compositor returned no frame");
                    ByteArrayOutputStream encoded=new ByteArrayOutputStream();
                    try {if(!bitmap.compress(Bitmap.CompressFormat.PNG,100,encoded))throw new IOException("Frame encoding failed");}
                    finally {bitmap.recycle();}
                    byte[] data=encoded.toByteArray();out.writeInt(data.length);out.write(data);
                } else if(op==2) {
                    long down=in.readLong(),time=in.readLong();int action=in.readInt(),count=in.readInt();
                    if(count<1 || count>10 || (action&255)>MotionEvent.ACTION_POINTER_UP || (action>>8)>=count)
                        throw new IOException("Invalid touch packet");
                    MotionEvent.PointerProperties[] properties=new MotionEvent.PointerProperties[count];
                    MotionEvent.PointerCoords[] coords=new MotionEvent.PointerCoords[count];
                    int ids=0;
                    for(int i=0;i<count;i++) {
                        int id=in.readInt();float x=in.readFloat(),y=in.readFloat();
                        if(id<0 || id>31 || (ids&(1<<id))!=0 || !Float.isFinite(x) || !Float.isFinite(y))throw new IOException("Invalid pointer");
                        ids|=1<<id;
                        properties[i]=new MotionEvent.PointerProperties();properties[i].id=id;
                        properties[i].toolType=MotionEvent.TOOL_TYPE_FINGER;
                        coords[i]=new MotionEvent.PointerCoords();coords[i].x=Math.max(0,Math.min(WIDTH-1,x));
                        coords[i].y=Math.max(0,Math.min(HEIGHT-1,y));coords[i].pressure=1;coords[i].size=1;
                    }
                    MotionEvent event=MotionEvent.obtain(down,time,action,count,properties,coords,0,0,1,1,0,0,InputDevice.SOURCE_TOUCHSCREEN,0);
                    try {out.writeBoolean(inject(event));} finally {event.recycle();}
                } else if(op==3) {
                    int code=in.readInt();
                    if(code!=KeyEvent.KEYCODE_HOME && code!=KeyEvent.KEYCODE_BACK && code!=KeyEvent.KEYCODE_APP_SWITCH)
                        throw new IOException("Unsupported navigation key");
                    long now=SystemClock.uptimeMillis();boolean accepted=true;
                    for(int action:new int[]{KeyEvent.ACTION_DOWN,KeyEvent.ACTION_UP})
                        accepted&=inject(new KeyEvent(now,SystemClock.uptimeMillis(),action,code,0,0,KeyCharacterMap.VIRTUAL_KEYBOARD,0,KeyEvent.FLAG_FROM_SYSTEM,InputDevice.SOURCE_KEYBOARD));
                    out.writeBoolean(accepted);
                } else if(op==4) {
                    File apk=new File("/data/pocket-import.apk");
                    PackageInfo info=context.getPackageManager().getPackageArchiveInfo(apk.getPath(),0);
                    if(info==null)throw new IOException("Invalid Android package");
                    FrameworkProbe.install(context,apk,info.packageName);
                    Intent launch=context.getPackageManager().getLaunchIntentForPackage(info.packageName);
                    if(launch!=null)FrameworkProbe.launch(launch);
                    out.writeUTF("Installed "+info.packageName);
                } else throw new IOException("Unknown control command");
                out.flush();
            }
        } catch(EOFException closed) { /* Normal viewer disconnect. */ }
        catch(Throwable error) {System.err.println("POCKET_CONTROL_CLIENT_CLOSED "+error);}
        finally {clients.release();}
    }
    public static void main(String[] args) {
        try {
            if(android.os.Process.myUid()!=1000)throw new IOException("Expected guest system role");
            context=FrameworkProbe.context();
            input=FrameworkProbe.service("input","android.hardware.input.IInputManager$Stub");
            inject=Class.forName("android.hardware.input.IInputManager").getMethod("injectInputEvent",InputEvent.class,int.class);
            FileDescriptor fd=new FileDescriptor();
            Field number=FileDescriptor.class.getDeclaredField("descriptor");number.setAccessible(true);
            number.setInt(fd,Integer.parseInt(System.getenv("ANDROID_SOCKET_pocket_control")));
            try(LocalServerSocket server=new LocalServerSocket(fd)) {
                System.out.println("POCKET_CONTROL_READY sdk=28 width="+WIDTH+" height="+HEIGHT);
                while(true) {
                    LocalSocket socket=server.accept();
                    if(!clients.tryAcquire()){socket.close();continue;}
                    new Thread(()->serve(socket),"PocketControlClient").start();
                }
            }
        } catch(Throwable error) {System.err.println("POCKET_CONTROL_FAILED "+error);error.printStackTrace();System.exit(1);}
    }
}
