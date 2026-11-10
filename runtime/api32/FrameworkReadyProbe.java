// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.content.Context;
import android.content.ContentProviderClient;
import android.content.ContentResolver;
import android.content.pm.PackageManager;
import android.bluetooth.BluetoothAdapter;
import android.hardware.Camera;
import android.hardware.camera2.CameraManager;
import android.hardware.display.DisplayManager;
import android.view.Display;
import android.database.Cursor;
import android.os.Looper;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.IInterface;
import android.os.SystemClock;
import android.os.UserManager;
import android.provider.Settings;
import android.system.Os;
import java.util.concurrent.*;

/** Checks actual user readiness and the stock provider as an external tool. */
public final class FrameworkReadyProbe {
    private static void checkSoftwareDisplay(Context context)throws Exception {
        Class<?> properties=Class.forName("android.os.SystemProperties");
        if(!"pocket".equals(properties.getMethod("get",String.class,String.class).invoke(null,"ro.hardware.hwcomposer","")))return;
        int expected=(Integer)properties.getMethod("getInt",String.class,int.class)
                .invoke(null,"ro.boot.redroid_fps",60);
        Display display=context.getSystemService(DisplayManager.class).getDisplay(Display.DEFAULT_DISPLAY);
        float actual=display==null?Float.NaN:display.getRefreshRate();
        if(!Float.isFinite(actual) || expected<1 || expected>120 || Math.abs(actual-expected)>0.1f)
            throw new AssertionError("Display refresh rate does not match the software compositor profile");
        System.out.println("API32_SOFTWARE_DISPLAY_PASS expected_hz="+expected+" actual_hz="+actual+" source=DisplayManager");
    }
    private static void checkCameraService(Context context)throws Exception {
        Class<?> registry=Class.forName("android.os.ServiceManager");
        long until=SystemClock.uptimeMillis()+15000;
        // Use the normal requesting lookup to activate a declared lazy daemon.
        IBinder binder=(IBinder)registry.getMethod("getService",String.class).invoke(null,"media.camera");
        while(binder==null && SystemClock.uptimeMillis()<until) {
            binder=(IBinder)registry.getMethod("checkService",String.class).invoke(null,"media.camera");
            if(binder==null)Thread.sleep(50);
        }
        if(binder==null || !binder.pingBinder() || !"android.hardware.ICameraService".equals(binder.getInterfaceDescriptor()))
            throw new AssertionError("Stock camera service unavailable");
        Class<?> camera=Class.forName("android.hardware.ICameraService");
        Object service=Class.forName("android.hardware.ICameraService$Stub").getMethod("asInterface",IBinder.class).invoke(null,binder);
        int legacy=(Integer)camera.getMethod("getNumberOfCameras",int.class).invoke(service,0);
        int all=(Integer)camera.getMethod("getNumberOfCameras",int.class).invoke(service,1);
        boolean host=(Boolean)Class.forName("android.os.SystemProperties").getMethod("getBoolean",String.class,boolean.class)
            .invoke(null,"ro.pocket.camera.enabled",false);
        int expected=0;
        if(host)try(android.net.LocalSocket socket=new android.net.LocalSocket()) {
            socket.connect(new android.net.LocalSocketAddress("/dev/socket/pocket_host",android.net.LocalSocketAddress.Namespace.FILESYSTEM));
            socket.setSoTimeout(5000);
            java.io.DataOutputStream out=new java.io.DataOutputStream(socket.getOutputStream());
            java.io.DataInputStream in=new java.io.DataInputStream(socket.getInputStream());
            out.writeInt(dev.kreijstal.aosppocket.bridge.HostWire.MAGIC);out.writeInt(dev.kreijstal.aosppocket.bridge.HostWire.CAMERAS);
            dev.kreijstal.aosppocket.bridge.HostWire.write(out,"{}".getBytes("UTF-8"));out.flush();
            if(in.readInt()!=0)throw new AssertionError("Host camera inventory query failed");
            org.json.JSONArray devices=new org.json.JSONObject(new String(dev.kreijstal.aosppocket.bridge.HostWire.read(in,65536),"UTF-8")).getJSONArray("devices");
            for(int i=0;i<devices.length();i++)if(devices.getJSONObject(i).getJSONArray("yuv420_sizes").length()>0)expected++;
        }
        if(legacy<0 || legacy>all || all!=expected || Camera.getNumberOfCameras()!=legacy ||
                context.getSystemService(CameraManager.class).getCameraIdList().length!=expected)
            throw new AssertionError("Camera inventory does not match its actual backend: expected="+expected+" legacy="+legacy+" all="+all);
        // An initialized empty service returns ILLEGAL_ARGUMENT, whereas an
        // uninitialized provider manager returns DISCONNECTED. Do not accept
        // Camera API1's zero fallback for a missing/broken service as a pass.
        try {
            camera.getMethod("getCameraCharacteristics",String.class,int.class).invoke(service,"pocket-missing",32);
            throw new AssertionError("Unknown camera ID accepted");
        }catch(java.lang.reflect.InvocationTargetException error) {
            Throwable cause=error.getCause();
            if(!cause.getClass().getName().equals("android.os.ServiceSpecificException") ||
                    cause.getClass().getField("errorCode").getInt(cause)!=3)throw error;
        }
        if(!binder.pingBinder())throw new AssertionError("Camera service died during enumeration");
        System.out.println("API32_CAMERA_SERVICE_PASS stock_daemon=12L initialized=1 binder=live api1_count="+legacy+" api2_count="+all+
            " missing_id=illegal_argument providers="+(host?"host":"absent")+" backend_count="+expected);
    }
    private static void checkHardwareProfile(Context context)throws Exception {
        PackageManager packages=context.getPackageManager();
        org.json.JSONObject profile=new org.json.JSONObject(new String(java.nio.file.Files.readAllBytes(
                java.nio.file.Paths.get("/system/etc/pocket-hardware-capabilities.json")),"UTF-8"));
        org.json.JSONObject features=profile.getJSONObject("features");
        boolean camera=profile.getJSONObject("backends").getBoolean("cameraProvider");
        int checked=0,declared=0;java.util.Iterator<String> names=features.keys();
        while(names.hasNext()) {
            String name=names.next();
            // Only the host camera provider backs declared features, and only the generic camera ones.
            if(features.getBoolean(name)) {
                if(!camera || !(name.equals("android.hardware.camera") || name.equals("android.hardware.camera.any")) ||
                        !packages.hasSystemFeature(name))
                    throw new AssertionError("Declared hardware feature has no matching backend: "+name);
                declared++;
            } else if(packages.hasSystemFeature(name))
                throw new AssertionError("Hardware feature has no matching backend: "+name);
            checked++;
        }
        if(checked!=13)throw new AssertionError("Software profile hardware inventory changed");
        System.out.println("API32_HARDWARE_FEATURES_PASS verified="+checked+" declared="+declared+" profile=software source=PackageManager");
        if(packages.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH) ||
                packages.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE))
            throw new AssertionError("Software profile advertises Bluetooth without an HCI backend");
        Object manager=Class.forName("android.os.ServiceManager").getMethod("checkService",String.class)
                .invoke(null,"bluetooth_manager");
        if(manager!=null || BluetoothAdapter.getDefaultAdapter()!=null)
            throw new AssertionError("Bluetooth service/adapter exists in the profile without a backend");
        System.out.println("API32_BLUETOOTH_ABSENT_PASS features=absent manager=absent default_adapter=null backend=unavailable");
    }
    private static void checkTimeoutProfile()throws Exception {
        Class<?> properties=Class.forName("android.os.SystemProperties");
        int property=(Integer)properties.getMethod("getInt",String.class,int.class)
            .invoke(null,"ro.hw_timeout_multiplier",1);
        String egl=(String)properties.getMethod("get",String.class).invoke(null,"ro.hardware.egl");
        if(!"swiftshader".equals(egl))return;
        int framework=android.os.Build.class.getField("HW_TIMEOUT_MULTIPLIER").getInt(null);
        if(property!=4 || framework!=property)
            throw new AssertionError("Expected software-emulation timeout multiplier 4, property="+property+" framework="+framework);
        System.out.println("API32_TIMEOUT_PROFILE_PASS hw_timeout_multiplier="+framework+" source=Build");
    }
    private static void checkMemory(Context context)throws Exception {
        Class<?> properties=Class.forName("android.os.SystemProperties");
        int limit=(Integer)properties.getMethod("getInt",String.class,int.class)
            .invoke(null,"ro.pocket.max_cached_processes",0);
        if(limit==0)return;
        if(limit<2 || limit>32)throw new AssertionError("Invalid guest cached process limit");
        Class<?> api=Class.forName("android.app.IActivityManager");
        Object manager=Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
        int override=(Integer)api.getMethod("getProcessLimit").invoke(manager);
        int id=context.getResources().getIdentifier("config_customizedMaxCachedProcesses","integer","android");
        if(id==0 || context.getResources().getInteger(id)!=limit || override!=-1)
            throw new AssertionError("Expected the framework resource default without a process-limit override");
        IBinder binder=(IBinder)Class.forName("android.os.ServiceManager").getMethod("getService",String.class)
                .invoke(null,"activity");
        java.io.File file=new java.io.File("/tmp/api32-memory-settings.txt");
        try(java.io.FileOutputStream output=new java.io.FileOutputStream(file)) {
            binder.dump(output.getFD(),new String[]{"settings"});
        }
        if(file.length()>256*1024)throw new AssertionError("ActivityManager settings dump exceeds bound");
        String dump=new String(java.nio.file.Files.readAllBytes(file.toPath()),"UTF-8");
        if(dump.contains("Permission Denial:") ||
                !java.util.regex.Pattern.compile("(?m)^\\s*mCustomizedMaxCachedProcesses="+limit+"\\s*$").matcher(dump).find() ||
                !java.util.regex.Pattern.compile("(?m)^\\s*CUR_MAX_CACHED_PROCESSES="+limit+"\\s*$").matcher(dump).find())
            throw new AssertionError("ActivityManager constructor/current defaults do not match the memory profile: "+dump);
        System.out.println("API32_GUEST_MEMORY_PROFILE_PASS cached_process_limit="+limit+" host_memory_policy=unchanged");
        System.out.println("API32_GUEST_MEMORY_EARLY_DEFAULT_PASS constructor_limit="+limit+" current_limit="+limit+
                " override="+override+" source=stock_ActivityManager_settings");
    }
    private static void checkSettings(Context context)throws Exception {
        // Like the platform's `content` command, this app_process tool has no
        // AMS ApplicationThread registration. Acquire a real external provider
        // reference; do not weaken AMS caller checks or fabricate an app record.
        Class<?> managerType=Class.forName("android.app.IActivityManager");
        Object manager=Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
        IBinder token=new Binder();Object provider=null;
        try {
            Object holder=managerType.getMethod("getContentProviderExternal",String.class,int.class,IBinder.class,String.class)
                    .invoke(manager,"settings",0,token,"pocket-api32-readiness");
            if(holder==null)throw new AssertionError("No external Settings provider");
            provider=holder.getClass().getField("provider").get(holder);
            IBinder binder=((IInterface)provider).asBinder();
            if(!binder.pingBinder() || !"android.content.IContentProvider".equals(binder.getInterfaceDescriptor()))
                throw new AssertionError("Settings provider Binder unavailable");
            Class<?> providerType=Class.forName("android.content.IContentProvider");
            try(ContentProviderClient client=(ContentProviderClient)ContentProviderClient.class.getConstructor(
                    ContentResolver.class,providerType,String.class,boolean.class)
                    .newInstance(context.getContentResolver(),provider,"settings",true)) {
                String key="pocket_api32_provider_probe_"+Os.getpid();
                String nonce=java.util.UUID.randomUUID().toString();
                Bundle args=new Bundle();args.putInt("_user",0);args.putString("value",nonce);
                try {
                    client.call("PUT_global",key,args);
                    Bundle value=client.call("GET_global",key,args);
                    if(value==null || !nonce.equals(value.getString("value")))
                        throw new AssertionError("Settings call round trip failed");
                    try(Cursor cursor=client.query(Settings.Global.getUriFor(key),new String[]{"name","value"},null,null,null)) {
                        if(cursor==null || !cursor.moveToFirst() || !key.equals(cursor.getString(0)) ||
                                !nonce.equals(cursor.getString(1)) || cursor.moveToNext())
                            throw new AssertionError("Settings query round trip failed");
                    }
                } finally {
                    if(client.delete(Settings.Global.getUriFor(key),null,null)!=1)
                        throw new AssertionError("Settings probe cleanup failed");
                }
                Bundle value=client.call("GET_global",key,args);
                if(value!=null && value.getString("value")!=null)
                    throw new AssertionError("Settings probe value survived cleanup");
                try(Cursor cursor=client.query(Settings.Global.getUriFor(key),new String[]{"name","value"},null,null,null)) {
                    if(cursor==null || cursor.moveToFirst())throw new AssertionError("Settings probe row survived cleanup");
                }
                System.out.println("API32_SETTINGS_PROVIDER_PASS stock_provider=1 client=external_tool call=real query=real nonce=verified cleanup=1");
            }
        } finally {
            if(provider!=null)managerType.getMethod("removeContentProviderExternalAsUser",String.class,IBinder.class,int.class)
                    .invoke(manager,"settings",token,0);
        }
    }
    public static void main(String[] args) {
        try {
            if(android.os.Build.VERSION.SDK_INT!=32 || Os.getuid()!=0)
                throw new AssertionError("Expected an isolated 12L root entry");
            Os.setgid(1000);Os.setuid(1000);
            if(Os.getuid()!=1000)throw new AssertionError("System role unavailable");
            Looper.prepare();
            Class<?> thread=Class.forName("android.app.ActivityThread");
            Object main=thread.getMethod("systemMain").invoke(null);
            Context context=(Context)thread.getMethod("getSystemContext").invoke(main);
            FutureTask<Void> task=new FutureTask<>(()->{
                checkTimeoutProfile();
                checkMemory(context);
                checkHardwareProfile(context);
                checkSoftwareDisplay(context);
                UserManager user=context.getSystemService(UserManager.class);
                long deadline=SystemClock.uptimeMillis()+60000;
                while(!user.isUserUnlocked()) {
                    if(SystemClock.uptimeMillis()>=deadline)throw new AssertionError("User 0 did not unlock after framework boot");
                    Thread.sleep(250);
                }
                System.out.println("API32_FRAMEWORK_USER_UNLOCKED user=0 source=UserManager");
                checkSettings(context);
                checkCameraService(context);
                return null;
            });
            Thread worker=new Thread(task,"api32-framework-ready");worker.setDaemon(true);worker.start();
            task.get(80,TimeUnit.SECONDS);
            System.exit(0);
        } catch(Throwable error) {
            error.printStackTrace();System.err.println("API32_FRAMEWORK_READY_FAILED");System.exit(1);
        }
    }
}
