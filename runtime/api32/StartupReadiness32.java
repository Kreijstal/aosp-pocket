// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.content.Context;
import android.app.KeyguardManager;
import android.os.Build;
import android.os.Environment;
import android.os.SystemClock;
import android.os.UserManager;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.system.Os;
import java.io.File;
import java.io.IOException;
import java.io.PrintWriter;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.json.JSONObject;

/** Read the pinned 12L boot completion state without changing broadcast scheduling. */
public final class StartupReadiness32 {
    private static volatile Observer observer;
    public static boolean selected()throws IOException {
        File marker=new File("/system/etc/pocket-app-readiness32");
        if(!marker.exists())return false;
        if(marker.length()>64 || !new String(Files.readAllBytes(marker.toPath()),StandardCharsets.US_ASCII)
                .equals("boot-broadcasts-and-volume-v1\n"))throw new IOException("Unsupported app readiness protocol");
        return true;
    }
    private static final class Observer {
        final Object controller;
        final Field completed;
        Observer()throws Exception {
            // This JAR is loaded by the control bridge outside the boot path.
            // Use the actual services class loader, never load another copy of
            // LocalServices and accidentally query an empty singleton registry.
            Field cached=Class.forName("com.android.internal.os.ZygoteInit")
                .getDeclaredField("sCachedSystemServerClassLoader");cached.setAccessible(true);
            Object actual=cached.get(null);
            if(!(actual instanceof ClassLoader))throw new IOException("System-server class loader is unavailable");
            ClassLoader loader=(ClassLoader)actual;
            Class<?> activity=Class.forName("com.android.server.am.ActivityManagerService",false,loader);
            Class<?> internal=Class.forName("android.app.ActivityManagerInternal",false,loader);
            Object local=Class.forName("com.android.server.LocalServices",false,loader)
                .getMethod("getService",Class.class).invoke(null,internal);
            if(local==null)throw new IOException("Local activity service is unavailable");
            // LocalService is a non-static AMS inner class in the pinned 12L
            // revision. Resolve its typed outer reference, not a compiler name.
            Object service=null;
            for(Field field:local.getClass().getDeclaredFields())if(field.getType()==activity) {
                if(service!=null)throw new IOException("Ambiguous local activity service");
                field.setAccessible(true);service=field.get(local);
            }
            if(service==null)throw new IOException("Expected the local Android12L activity service");
            Field user=activity.getDeclaredField("mUserController");user.setAccessible(true);
            controller=user.get(service);
            if(controller==null)throw new IOException("Local user controller is unavailable");
            completed=controller.getClass().getDeclaredField("mBootCompleted");
            if(completed.getType()!=boolean.class || !Modifier.isVolatile(completed.getModifiers()))
                throw new IOException("Unexpected Android12L boot completion ABI");
            completed.setAccessible(true);
        }
    }
    private static Observer observer()throws Exception {
        Observer value=observer;
        if(value==null)synchronized(StartupReadiness32.class) {
            value=observer;if(value==null)observer=value=new Observer();
        }
        return value;
    }
    public static JSONObject state(Context context)throws Exception {
        if(Build.VERSION.SDK_INT!=32 || Os.getuid()!=1000 || context==null)
            throw new IOException("Expected SDK32 system-server readiness context");
        Observer value=observer();
        boolean boot=value.completed.getBoolean(value.controller);
        boolean unlocked=context.getSystemService(UserManager.class).isUserUnlocked();
        KeyguardManager keyguard=context.getSystemService(KeyguardManager.class);
        boolean credentialRequired=!unlocked && keyguard.isDeviceSecure() && keyguard.isDeviceLocked();
        boolean external=new File("/system/etc/pocket-external-volume32").isFile(),mounted=false;
        if(external)for(StorageVolume volume:context.getSystemService(StorageManager.class).getStorageVolumes())
            if(volume.isPrimary() && volume.isEmulated() && Environment.MEDIA_MOUNTED.equals(volume.getState()))mounted=true;
        return new JSONObject().put("schema",1).put("sdk",32).put("uid",Os.getuid()).put("pid",Os.getpid())
            .put("source","LocalServices.UserController.mBootCompleted").put("uptimeMillis",SystemClock.uptimeMillis())
            .put("classLoaderSource","ZygoteInit.sCachedSystemServerClassLoader")
            .put("bootBroadcastComplete",boot).put("userUnlocked",unlocked)
            .put("credentialRequired",credentialRequired)
            .put("externalVolumeRequired",external).put("primaryVolumeMounted",mounted)
            .put("readyForApps",boot && unlocked && (!external || mounted));
    }
    public static void await(Context context,PrintWriter log)throws Exception {
        long start=SystemClock.uptimeMillis(),deadline=start+120000;boolean waiting=false;
        for(;;) {
            JSONObject state=state(context);
            if(state.getBoolean("credentialRequired"))throw new IOException("Unlock Android before using apps");
            if(state.getBoolean("readyForApps")) {
                String proof="POCKET_APP_READINESS32_READY "+state.put("waitedMillis",SystemClock.uptimeMillis()-start);
                log.println(proof);System.out.println(proof);return;
            }
            if(!waiting){log.println("POCKET_APP_READINESS32_WAIT "+state);log.flush();waiting=true;}
            if(SystemClock.uptimeMillis()>=deadline)throw new IOException("Android app startup did not finish");
            Thread.sleep(100);
        }
    }
}
