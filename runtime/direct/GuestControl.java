// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.content.*;
import android.content.pm.PackageInfo;
import android.net.*;
import android.os.*;
import android.view.*;
import java.io.*;
import java.lang.reflect.*;
import java.util.concurrent.Semaphore;
import android.util.DisplayMetrics;
import dev.kreijstal.aosppocket.bridge.DisplayGeometry;

/** App-private host connection to the real Android 9 compositor/input services. */
public final class GuestControl {
    private static DisplayGeometry display;
    private static final Semaphore clients=new Semaphore(4);
    private static Object input;
    private static Method inject;
    private static Context context;
    private static String failureMessage(Exception error) {
        Throwable detail=error;
        while(detail instanceof InvocationTargetException && detail.getCause()!=null)detail=detail.getCause();
        String message=detail.getMessage();
        if(message==null || message.isEmpty())message=detail.getClass().getSimpleName();
        // DataOutputStream.writeUTF has a 65535-byte limit, including modified UTF-8.
        return message.length()>8000 ? message.substring(0,8000) : message;
    }
    private static String installPackage() throws Exception {
        File staged=new File("/data/pocket-import"),base=new File(staged,"base.apk");
        if(!base.isFile() || base.length()==0)throw new IOException("The imported base APK is missing or empty.");
        File[] apks=staged.listFiles((directory,name)->name.endsWith(".apk"));
        PackageInfo info=context.getPackageManager().getPackageArchiveInfo(base.getPath(),0);
        if(info==null) {
            // getPackageArchiveInfo discards PackageParser's rejection reason.
            // Recover it on the actual guest instead of guessing at incompatibility.
            Class<?> parserClass=Class.forName("android.content.pm.PackageParser");
            Object parser=parserClass.getDeclaredConstructor().newInstance();
            try {parserClass.getMethod("parsePackage",File.class,int.class).invoke(parser,base,0);}
            catch(InvocationTargetException rejected) {
                throw new IOException("Cannot install this APK: "+failureMessage(rejected),rejected.getCause());
            }
            throw new IOException("Android could not read this APK's package information.");
        }
        FrameworkProbe.install(context,apks,info.packageName);
        Intent launch=context.getPackageManager().getLaunchIntentForPackage(info.packageName);
        if(launch!=null)FrameworkProbe.launch(launch);
        return "Installed "+info.packageName;
    }
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
            display.write(out);out.flush();
            while(true) {
                int op=in.readInt();
                if(op==2) {
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
                        coords[i]=new MotionEvent.PointerCoords();coords[i].x=Math.max(0,Math.min(display.width-1,x));
                        coords[i].y=Math.max(0,Math.min(display.height-1,y));coords[i].pressure=1;coords[i].size=1;
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
                    // The host stages the base APK and any split APKs together in one folder.
                    try {out.writeUTF(installPackage());}
                    catch(Exception rejected) {
                        String message=failureMessage(rejected);
                        System.err.println("POCKET_INSTALL_FAILED "+message);
                        out.writeUTF("Install failed: "+message);
                    }
                } else if(op==5) {
                    try {out.writeUTF(HostDevices.importContacts(context));}
                    catch(Exception error){out.writeUTF("Contacts import unavailable: "+error.getMessage());}
                } else if(op==6) {
                    String url=in.readUTF();
                    org.json.JSONObject result=HostDevices.query(dev.kreijstal.aosppocket.bridge.HostWire.INFO,new org.json.JSONObject());
                    result.put("guest_elapsed_ms",SystemClock.elapsedRealtime());
                    if(!url.isEmpty()) {
                        java.net.HttpURLConnection connection=(java.net.HttpURLConnection)new java.net.URL(url).openConnection();
                        connection.setConnectTimeout(10000);connection.setReadTimeout(10000);
                        try {result.put("http_status",connection.getResponseCode());
                            try(InputStream body=connection.getInputStream()) {byte[] bytes=new byte[256];int count=body.read(bytes);
                                result.put("http_body",count<0?"":new String(bytes,0,count,"UTF-8"));}}
                        catch(Exception error){result.put("http_error",error.toString());}
                        finally {connection.disconnect();}
                    }
                    android.net.ConnectivityManager cm=(android.net.ConnectivityManager)context.getSystemService(Context.CONNECTIVITY_SERVICE);
                    result.put("guest_network_connected",cm.getActiveNetworkInfo()!=null && cm.getActiveNetworkInfo().isConnected());
                    out.writeUTF(result.toString());
                } else if(op==7) {
                    out.writeUTF(HostDevices.diagnose(context).toString());
                } else if(op==8) {
                    FrameworkProbe.launch(new Intent(Intent.ACTION_DIAL,Uri.fromParts("tel","",null)));
                    out.writeBoolean(true);
                } else if(op==9) {
                    String name=in.readUTF();
                    Intent launch=context.getPackageManager().getLaunchIntentForPackage(name);
                    if(launch==null)throw new IOException("App has no launcher activity: "+name);
                    FrameworkProbe.launch(launch);out.writeBoolean(true);
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
            DisplayMetrics metrics=new DisplayMetrics();
            ((WindowManager)context.getSystemService(Context.WINDOW_SERVICE)).getDefaultDisplay().getRealMetrics(metrics);
            display=new DisplayGeometry(metrics.widthPixels,metrics.heightPixels,metrics.densityDpi);
            new HostNetwork(context);
            input=FrameworkProbe.service("input","android.hardware.input.IInputManager$Stub");
            inject=Class.forName("android.hardware.input.IInputManager").getMethod("injectInputEvent",InputEvent.class,int.class);
            FileDescriptor fd=new FileDescriptor();
            Field number=FileDescriptor.class.getDeclaredField("descriptor");number.setAccessible(true);
            number.setInt(fd,Integer.parseInt(System.getenv("ANDROID_SOCKET_pocket_control")));
            try(LocalServerSocket server=new LocalServerSocket(fd)) {
                System.out.println("POCKET_CONTROL_READY sdk=28 width="+display.width+" height="+display.height+" density="+display.densityDpi);
                while(true) {
                    LocalSocket socket=server.accept();
                    if(!clients.tryAcquire()){socket.close();continue;}
                    new Thread(()->serve(socket),"PocketControlClient").start();
                }
            }
        } catch(Throwable error) {System.err.println("POCKET_CONTROL_FAILED "+error);error.printStackTrace();System.exit(1);}
    }
}
