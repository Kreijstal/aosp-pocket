// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.content.Context;
import android.content.Intent;
import android.content.pm.*;
import android.os.*;
import android.system.Os;
import java.io.*;
import java.lang.reflect.*;
import java.nio.file.Files;
import java.security.MessageDigest;
import org.json.JSONObject;
import org.json.JSONArray;

/** Test-only observer of the stock framework; never changes application state. */
public final class AppInspector32 {
    private static String dump(String service,String... arguments)throws Exception {
        IBinder binder=(IBinder)Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,service);
        if(binder==null)throw new IOException("Missing service "+service);
        File file=new File("/data/local/tmp/pocket-app-inspect-"+service+".txt");
        try(FileOutputStream out=new FileOutputStream(file)){binder.dump(out.getFD(),arguments);}
        if(file.length()>1024*1024)throw new IOException("Framework dump exceeds test limit");
        return new String(Files.readAllBytes(file.toPath()),"UTF-8");
    }
    private static String hex(byte[] bytes) {
        StringBuilder out=new StringBuilder();for(byte b:bytes)out.append(String.format("%02x",b&255));return out.toString();
    }
    private static String fileDigest(File file)throws Exception {
        MessageDigest hash=MessageDigest.getInstance("SHA-256");byte[] bytes=new byte[65536];
        try(InputStream input=new FileInputStream(file)){int n;while((n=input.read(bytes))!=-1)hash.update(bytes,0,n);}return hex(hash.digest());
    }
    private static void queryExternalPrimary(JSONObject result)throws Exception {
        // This app_process observer has no registered IApplicationThread.
        // Use the stock `content` command's external-owner token contract;
        // passing systemMain's unregistered thread to ContentResolver fails
        // before MediaProvider is contacted. This proves system observation,
        // not an ordinary application's MediaStore permissions.
        Class<?> activityManager=Class.forName("android.app.IActivityManager");
        Object manager=Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
        IBinder token=new Binder();Object holder=null;
        try {
            holder=activityManager.getMethod("getContentProviderExternal",String.class,int.class,IBinder.class,String.class)
                .invoke(manager,"media",0,token,"pocket-storage-observer");
            if(holder==null)throw new IOException("MediaProvider external acquisition returned null");
            Object provider=holder.getClass().getField("provider").get(holder);
            if(provider==null)throw new IOException("MediaProvider external acquisition has no provider");
            android.content.AttributionSource attribution=new android.content.AttributionSource.Builder(Os.getuid()).setPackageName("android").build();
            Bundle extras=new Bundle();extras.putString(android.content.ContentResolver.QUERY_ARG_SQL_SELECTION,"1=0");
            Method query=Class.forName("android.content.IContentProvider").getMethod("query",android.content.AttributionSource.class,
                android.net.Uri.class,String[].class,Bundle.class,Class.forName("android.os.ICancellationSignal"));
            try(android.database.Cursor cursor=(android.database.Cursor)query.invoke(provider,attribution,
                    android.provider.MediaStore.Files.getContentUri("external_primary"),new String[]{"_id"},extras,null)) {
                if(cursor==null)throw new IOException("MediaProvider query returned null");
                int rows=cursor.getCount();if(rows!=0)throw new IOException("Zero-row query returned rows");
                result.put("externalPrimaryQuery",true).put("externalPrimaryQueryRows",rows).put("externalPrimaryQueryOwner","system-external-token");
            }
        }finally {
            if(holder!=null)activityManager.getMethod("removeContentProviderExternalAsUser",String.class,IBinder.class,int.class).invoke(manager,"media",token,0);
        }
    }
    private static void inspectDeviceProfile(Context context,JSONObject result)throws Exception {
        File policy=new File("/system/etc/pocket-device-profile.json");if(!policy.isFile())return;
        JSONObject profile=new JSONObject(new String(Files.readAllBytes(policy.toPath()),"UTF-8"));
        PackageManager pm=context.getPackageManager();boolean multidisplayInstalled;
        try{pm.getApplicationInfo("com.android.emulator.multidisplay",0);multidisplayInstalled=true;}
        catch(PackageManager.NameNotFoundException expected){multidisplayInstalled=false;}
        PackageInfo dynamic=pm.getPackageInfo("com.android.dynsystem",0);
        ActivityInfo receiver=pm.getReceiverInfo(new android.content.ComponentName("com.android.dynsystem","com.android.dynsystem.BootCompletedReceiver"),PackageManager.MATCH_DISABLED_COMPONENTS);
        java.util.List<android.app.ActivityManager.RunningAppProcessInfo> processes=context.getSystemService(android.app.ActivityManager.class).getRunningAppProcesses();
        if(processes==null)throw new IOException("Cannot inspect running device-profile packages");
        JSONArray unsupported=new JSONArray();
        for(android.app.ActivityManager.RunningAppProcessInfo process:processes)
            if(process.processName.equals("com.android.emulator.multidisplay") || process.processName.equals("com.android.dynsystem") || process.processName.startsWith("com.android.dynsystem:"))unsupported.put(process.processName);
        result.put("deviceProfile",new JSONObject().put("profile",profile.getString("profile")).put("policySha256",fileDigest(policy))
            .put("multidisplayInstalled",multidisplayInstalled).put("dynamicSystemPackagePresent",true)
            .put("dynamicSystemUid",dynamic.applicationInfo.uid).put("dynamicSystemApkSha256",fileDigest(new File(dynamic.applicationInfo.sourceDir)))
            .put("dynamicSystemBootReceiverEnabled",receiver.enabled).put("runningUnsupportedPackages",unsupported));
    }
    public static void main(String[] args) {
        try {
            if(Build.VERSION.SDK_INT!=32 || Os.getuid()!=0 || args.length!=1)throw new IOException("Expected SDK32 owner entry");
            if(args[0].equals("--manager-peer-denied")) {
                try(android.net.LocalSocket socket=new android.net.LocalSocket()) {
                    socket.connect(new android.net.LocalSocketAddress("/dev/socket/pocket_control32",android.net.LocalSocketAddress.Namespace.FILESYSTEM));
                    socket.setSoTimeout(5000);
                    if(socket.getInputStream().read()!=-1)throw new IOException("Guest child received an owner control greeting");
                }
                System.out.println("POCKET_MANAGER32_PEER_DENIED "+new JSONObject().put("pid",Os.getpid()).put("uid",Os.getuid()).put("denied",true));
                System.out.flush();System.exit(0);return;
            }
            Os.setgid(1000);Os.setuid(1000);Looper.prepare();
            Class<?> api=Class.forName("android.app.ActivityThread");Object thread=api.getMethod("systemMain").invoke(null);
            Context context=(Context)api.getMethod("getSystemContext").invoke(thread);
            if(args[0].equals("--activity-policy")) {
                Class<?> properties=Class.forName("android.os.SystemProperties");
                int declared=(Integer)properties.getMethod("getInt",String.class,int.class).invoke(null,"ro.pocket.max_cached_processes",0);
                Class<?> activityManager=Class.forName("android.app.IActivityManager");
                Object manager=Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
                int override=(Integer)activityManager.getMethod("getProcessLimit").invoke(manager);
                int id=context.getResources().getIdentifier("config_customizedMaxCachedProcesses","integer","android");
                if(id==0)throw new IOException("No framework cached-process resource");
                JSONObject policy=new JSONObject().put("diagnostic","activity-policy").put("sdk",32).put("uid",Os.getuid()).put("pid",Os.getpid())
                    .put("uptimeMillis",SystemClock.uptimeMillis()).put("declaredLimit",declared).put("resourceLimit",context.getResources().getInteger(id))
                    .put("processLimitOverride",override).put("activitySettings",dump("activity","settings")).put("activityOom",dump("activity","oom"));
                System.out.println("POCKET_APP32_INSPECT "+policy);System.out.flush();System.exit(0);return;
            }
            PackageInfo info=context.getPackageManager().getPackageInfo(args[0],PackageManager.GET_SIGNING_CERTIFICATES);
            android.content.pm.Signature[] signatures=info.signingInfo.getApkContentsSigners();
            if(signatures.length!=1)throw new IOException("Unexpected signer count");
            ResolveInfo home=context.getPackageManager().resolveActivity(new Intent(Intent.ACTION_MAIN)
                .addCategory(Intent.CATEGORY_HOME),PackageManager.MATCH_DEFAULT_ONLY);
            JSONObject result=new JSONObject().put("sdk",Build.VERSION.SDK_INT).put("package",info.packageName)
                .put("uid",info.applicationInfo.uid).put("minSdk",info.applicationInfo.minSdkVersion)
                .put("targetSdk",info.applicationInfo.targetSdkVersion).put("versionCode",info.getLongVersionCode())
                .put("splitCount",info.splitNames==null?0:info.splitNames.length)
                .put("homePackage",home==null || home.activityInfo==null?"":home.activityInfo.packageName)
                .put("certificateSha256",hex(MessageDigest.getInstance("SHA-256").digest(signatures[0].toByteArray())))
                .put("activities",dump("activity","activities")).put("windows",dump("window","windows"));
            inspectDeviceProfile(context,result);
            JSONArray volumes=new JSONArray();boolean primary=false;
            for(android.os.storage.StorageVolume volume:context.getSystemService(android.os.storage.StorageManager.class).getStorageVolumes()) {
                volumes.put(new JSONObject().put("primary",volume.isPrimary()).put("emulated",volume.isEmulated()).put("state",volume.getState()).put("path",volume.getDirectory()==null?"":volume.getDirectory().getPath()));
                if(volume.isPrimary() && volume.isEmulated() && Environment.MEDIA_MOUNTED.equals(volume.getState()))primary=true;
            }
            result.put("storageVolumes",volumes).put("primaryEmulatedMounted",primary);
            if(new File("/system/etc/pocket-external-volume32").isFile()) {
                PackageInfo provider=context.getPackageManager().getPackageInfo("com.android.providers.media.module",PackageManager.GET_SIGNING_CERTIFICATES);
                android.content.pm.Signature[] certs=provider.signingInfo.getApkContentsSigners();if(certs.length!=1)throw new IOException("Unexpected MediaProvider signer count");
                result.put("mediaProvider",new JSONObject().put("package",provider.packageName).put("uid",provider.applicationInfo.uid)
                    .put("sourceDir",provider.applicationInfo.sourceDir).put("sha256",fileDigest(new File(provider.applicationInfo.sourceDir)))
                    .put("certificateSha256",hex(MessageDigest.getInstance("SHA-256").digest(certs[0].toByteArray()))));
            }
            if(primary)try{queryExternalPrimary(result);}
            catch(Exception error){while(error instanceof InvocationTargetException && error.getCause() instanceof Exception)error=(Exception)error.getCause();result.put("externalPrimaryQuery",false).put("externalPrimaryQueryError",error.toString());}
            if(info.packageName.equals("dev.kreijstal.pocketvolume32")) {
                String activity=dump("activity",info.packageName+"/.MainActivity");
                for(String line:activity.split("\n"))if(line.trim().startsWith("POCKET_VOLUME_APP32=")) {
                    result.put("externalVolumeApp",new JSONObject(new String(android.util.Base64.decode(line.trim().substring("POCKET_VOLUME_APP32=".length()),android.util.Base64.DEFAULT),"UTF-8")));
                }
                if(!result.has("externalVolumeApp"))result.put("externalVolumeAppError","Registered storage Activity returned no report").put("externalVolumeAppDump",activity.substring(0,Math.min(activity.length(),8192)));
            }
            if(info.packageName.equals("dev.kreijstal.pocketvolumeowner32")) {
                String activity=dump("activity",info.packageName+"/.MainActivity");
                for(String line:activity.split("\n"))if(line.trim().startsWith("POCKET_VOLUME_OWNER32=")) {
                    result.put("externalVolumeOwner",new JSONObject(new String(android.util.Base64.decode(line.trim().substring("POCKET_VOLUME_OWNER32=".length()),android.util.Base64.DEFAULT),"UTF-8")));
                }
                if(!result.has("externalVolumeOwner"))result.put("externalVolumeOwnerError","Owner Activity returned no report").put("externalVolumeOwnerDump",activity.substring(0,Math.min(activity.length(),8192)));
            }
            if(info.packageName.equals("dev.kreijstal.pocketcodec32test") || info.packageName.equals("dev.kreijstal.pocketav32test")) {
                boolean av=info.packageName.equals("dev.kreijstal.pocketav32test");String marker=av?"POCKET_AV_APP32=":"POCKET_CODEC_APP32=";
                String key=av?"mediaAvProbe":"mediaCodecProbe";
                long deadline=SystemClock.elapsedRealtime()+30000;JSONObject codec=null;
                do {
                    String activity=dump("activity",info.packageName+"/.MainActivity");
                    for(String line:activity.split("\n"))if(line.trim().startsWith(marker))
                        codec=new JSONObject(new String(android.util.Base64.decode(line.trim().substring(marker.length()),android.util.Base64.DEFAULT),"UTF-8"));
                    if(codec!=null && (codec.optString("validation").equals("PASS") || codec.optString("validation").equals("FAIL")))break;
                    Thread.sleep(250);
                }while(SystemClock.elapsedRealtime()<deadline);
                if(codec==null)result.put(key+"Error","Registered ordinary media Activity returned no report");
                else result.put(key,codec);
                PackageInfo permissions=context.getPackageManager().getPackageInfo(info.packageName,PackageManager.GET_PERMISSIONS);
                result.put(av?"avRequestedPermissions":"codecRequestedPermissions",new JSONArray(java.util.Arrays.asList(permissions.requestedPermissions==null?new String[0]:permissions.requestedPermissions)))
                    .put("activities",dump("activity","activities")).put("windows",dump("window","windows"));
            }
            System.out.println("POCKET_APP32_INSPECT "+result);System.out.flush();System.exit(0);
        }catch(Throwable error) {
            while(error instanceof InvocationTargetException && error.getCause()!=null)error=error.getCause();
            System.err.println("POCKET_APP32_INSPECT_ERROR "+error);error.printStackTrace();System.exit(1);
        }
    }
}
