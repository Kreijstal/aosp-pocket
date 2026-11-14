// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;
import android.content.*;
import android.os.*;
import android.system.Os;
import java.io.*;

/** Test-only system observer of an ordinary, AMS-hosted SDK32 provider. */
public final class CeAppInspector32 {
    private static String dump(String service,String... args)throws Exception {
        IBinder binder=(IBinder)Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,service);
        if(binder==null)throw new IOException("Missing diagnostic service "+service);
        File file=File.createTempFile("ce32-"+service+"-",".txt",new File("/tmp"));
        try {
            try(FileOutputStream out=new FileOutputStream(file)){binder.dump(out.getFD(),args);}
            if(file.length()>1024*1024)throw new IOException("Diagnostic dump exceeds limit");
            return new String(java.nio.file.Files.readAllBytes(file.toPath()),"UTF-8");
        }finally{java.nio.file.Files.deleteIfExists(file.toPath());}
    }
    private static int theaterMode(Context context)throws Exception {
        // app_process has no AMS-registered IApplicationThread. Use the same
        // external-owner contract as the content command and other probes.
        Class<?> api=Class.forName("android.app.IActivityManager");
        Object manager=Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);IBinder token=new Binder();
        Object holder=api.getMethod("getContentProviderExternal",String.class,int.class,IBinder.class,String.class)
            .invoke(manager,"settings",0,token,"pocket-ce-power-diagnostic");
        if(holder==null)throw new IOException("Settings external acquisition returned null");
        try {
            Object provider=holder.getClass().getField("provider").get(holder);
            try(ContentProviderClient client=(ContentProviderClient)ContentProviderClient.class.getConstructor(ContentResolver.class,
                    Class.forName("android.content.IContentProvider"),String.class,boolean.class)
                    .newInstance(context.getContentResolver(),provider,"settings",true)) {
                Bundle extras=new Bundle();extras.putInt("_user",0);
                Bundle reply=client.call("GET_global","theater_mode_on",extras);
                if(reply==null)throw new IOException("Settings returned no diagnostic result");
                String value=reply.getString("value");return value==null?0:Integer.parseInt(value);
            }
        }finally{api.getMethod("removeContentProviderExternalAsUser",String.class,IBinder.class,int.class).invoke(manager,"settings",token,0);}
    }
    private static void powerDiagnostic(Context context,org.json.JSONObject state)throws Exception {
        for(String service:new String[]{"power","input","window"}) {
            String key=service.equals("window")?"policyDump":service+"Dump";
            try{state.put(key,service.equals("window")?dump(service,"policy"):dump(service));}
            catch(Exception error){state.put(key+"Error",error.toString());}
        }
        try{state.put("theaterMode",theaterMode(context));}
        catch(Exception error){state.put("theaterModeError",error.toString());}
        state.put("observerUid",Os.getuid()).put("observerPid",Os.getpid()).put("readOnly",true);
    }
    private static org.json.JSONObject externalStorage(Context context,boolean query)throws Exception {
        return externalStorage(context,query,SystemClock.elapsedRealtime()+60000);
    }
    private static org.json.JSONObject externalStorage(Context context,boolean query,long deadline)throws Exception {
        boolean primary;org.json.JSONArray volumes;long started=SystemClock.elapsedRealtime();
        do {
            primary=false;volumes=new org.json.JSONArray();
            for(android.os.storage.StorageVolume volume:context.getSystemService(android.os.storage.StorageManager.class).getStorageVolumes()) {
                volumes.put(new org.json.JSONObject().put("primary",volume.isPrimary()).put("emulated",volume.isEmulated()).put("state",volume.getState()));
                if(volume.isPrimary() && volume.isEmulated() && Environment.MEDIA_MOUNTED.equals(volume.getState()))primary=true;
            }
            if(!query || primary || SystemClock.elapsedRealtime()>=deadline)break;
            Thread.sleep(100);
        }while(true);
        org.json.JSONObject result=new org.json.JSONObject().put("primaryEmulatedMounted",primary).put("volumes",volumes)
            .put("mountWaitMillis",SystemClock.elapsedRealtime()-started);
        if(!query)return result;
        if(!primary)throw new IOException("Unlocked primary emulated volume is not mounted");
        Class<?> api=Class.forName("android.app.IActivityManager");
        Object manager=Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);IBinder token=new Binder();
        Object holder=api.getMethod("getContentProviderExternal",String.class,int.class,IBinder.class,String.class).invoke(manager,"media",0,token,"pocket-ce-volume-test");
        if(holder==null)throw new IOException("Unlocked MediaProvider external acquisition returned null");
        try {
            Object provider=holder.getClass().getField("provider").get(holder);
            android.content.AttributionSource attribution=new android.content.AttributionSource.Builder(Os.getuid()).setPackageName("android").build();
            Bundle extras=new Bundle();extras.putString(ContentResolver.QUERY_ARG_SQL_SELECTION,"1=0");
            java.lang.reflect.Method method=Class.forName("android.content.IContentProvider").getMethod("query",android.content.AttributionSource.class,
                android.net.Uri.class,String[].class,Bundle.class,Class.forName("android.os.ICancellationSignal"));
            long queryStarted=SystemClock.elapsedRealtime();int notReady=0;
            while(true) {
                try(android.database.Cursor cursor=(android.database.Cursor)method.invoke(provider,attribution,
                        android.provider.MediaStore.Files.getContentUri("external_primary"),new String[]{"_id"},extras,null)) {
                    if(cursor==null || cursor.getCount()!=0)throw new IOException("Unlocked zero-row MediaProvider query failed");
                    result.put("externalPrimaryQuery",true).put("externalPrimaryQueryRows",0).put("queryOwner","system-external-token")
                        .put("externalPrimaryQueryWaitMillis",SystemClock.elapsedRealtime()-queryStarted).put("volumeNotReadyRetries",notReady);
                    break;
                }catch(java.lang.reflect.InvocationTargetException error) {
                    // StorageManager's mounted notification precedes the
                    // provider's asynchronous volume/database attachment.
                    Throwable cause=error.getCause();
                    if(!(cause instanceof IllegalArgumentException) || !"Volume external_primary not found".equals(cause.getMessage()))throw error;
                    if(SystemClock.elapsedRealtime()>=deadline)throw new IOException("Mounted external_primary did not become queryable before the unlock deadline",error);
                    notReady++;Thread.sleep(100);
                }
            }
        }finally{api.getMethod("removeContentProviderExternalAsUser",String.class,IBinder.class,int.class).invoke(manager,"media",token,0);}
        return result;
    }
    /** Read only view attributes. Never read password text or inject input. */
    private static org.json.JSONObject passwordEntry()throws Exception {
        HandlerThread callbacks=new HandlerThread("Ce32UiObserver");callbacks.start();
        Object automation=null;boolean connected=false;
        Class<?> type=Class.forName("android.app.UiAutomation");
        try {
            Class<?> connection=Class.forName("android.app.IUiAutomationConnection");
            Object local=Class.forName("android.app.UiAutomationConnection").getConstructor().newInstance();
            automation=type.getConstructor(Looper.class,connection).newInstance(callbacks.getLooper(),local);
            int flags=type.getField("FLAG_DONT_SUPPRESS_ACCESSIBILITY_SERVICES").getInt(null);
            type.getMethod("connect",int.class).invoke(automation,flags);connected=true;
            android.accessibilityservice.AccessibilityServiceInfo serviceInfo=
                (android.accessibilityservice.AccessibilityServiceInfo)type.getMethod("getServiceInfo").invoke(automation);
            if(serviceInfo==null)throw new IOException("Missing accessibility observer service info");
            serviceInfo.flags|=android.accessibilityservice.AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS;
            type.getMethod("setServiceInfo",android.accessibilityservice.AccessibilityServiceInfo.class).invoke(automation,serviceInfo);
            org.json.JSONArray packages=new org.json.JSONArray();
            org.json.JSONObject found=null;int count=0;
            long started=SystemClock.elapsedRealtime(),deadline=started+3000;
            do {
                // Enabling interactive windows schedules their computation in
                // system_server; the first result may still be empty/IME-only.
                @SuppressWarnings("unchecked")
                java.util.List<android.view.accessibility.AccessibilityWindowInfo> windows=
                    (java.util.List<android.view.accessibility.AccessibilityWindowInfo>)type.getMethod("getWindows").invoke(automation);
                packages=new org.json.JSONArray();count=0;
                try {
                    if(windows.size()>32)throw new IOException("Too many accessibility windows");
                    for(android.view.accessibility.AccessibilityWindowInfo window:windows) {
                        android.view.accessibility.AccessibilityNodeInfo root=window.getRoot();if(root==null)continue;
                        try {
                            String owner=String.valueOf(root.getPackageName());packages.put(owner);
                            if(!owner.equals("com.android.systemui"))continue;
                            java.util.List<android.view.accessibility.AccessibilityNodeInfo> nodes=
                                root.findAccessibilityNodeInfosByViewId("com.android.systemui:id/passwordEntry");
                            try {
                                if(nodes.size()>16)throw new IOException("Too many password-entry nodes");
                                for(android.view.accessibility.AccessibilityNodeInfo node:nodes) {
                                    count++;if(!node.isVisibleToUser())continue;
                                    android.graphics.Rect bounds=new android.graphics.Rect();node.getBoundsInScreen(bounds);
                                    found=new org.json.JSONObject().put("resourceId",node.getViewIdResourceName())
                                        .put("className",String.valueOf(node.getClassName())).put("visible",node.isVisibleToUser())
                                        .put("enabled",node.isEnabled()).put("focused",node.isFocused()).put("password",node.isPassword())
                                        .put("editable",node.isEditable()).put("windowType",window.getType())
                                        .put("bounds",new org.json.JSONArray().put(bounds.left).put(bounds.top).put(bounds.right).put(bounds.bottom));
                                }
                            }finally{for(android.view.accessibility.AccessibilityNodeInfo node:nodes)node.recycle();}
                        }finally{root.recycle();}
                    }
                }finally{for(android.view.accessibility.AccessibilityWindowInfo window:windows)window.recycle();}
                if(found!=null || SystemClock.elapsedRealtime()>=deadline)break;
                Thread.sleep(50);
            }while(true);
            return new org.json.JSONObject().put("readOnly",true).put("passwordTextRead",false)
                .put("interactiveWindowsRequested",true)
                .put("windowWaitMillis",SystemClock.elapsedRealtime()-started)
                .put("windowPackages",packages).put("matchingNodes",count)
                .put("passwordEntry",found==null?org.json.JSONObject.NULL:found);
        }finally {
            try{if(connected)type.getMethod("disconnect").invoke(automation);}
            finally{callbacks.quitSafely();callbacks.join(1000);}
        }
    }
    private static org.json.JSONObject keyguardState(Context context,String mode)throws Exception {
        android.app.KeyguardManager keyguard=context.getSystemService(android.app.KeyguardManager.class);
        org.json.JSONObject state=new org.json.JSONObject().put("secure",keyguard.isDeviceSecure())
            .put("deviceLocked",keyguard.isDeviceLocked()).put("keyguardLocked",keyguard.isKeyguardLocked())
            .put("interactive",context.getSystemService(PowerManager.class).isInteractive())
            .put("uptimeMillis",SystemClock.uptimeMillis()).put("externalStorage",externalStorage(context,false));
        if(mode.equals("keyguard-ui")) {
            state.put("ui",passwordEntry());
            // Accessibility connection/window retrieval may span the
            // keyguard's sleep timeout. Gate input on the current state.
            state.put("interactive",context.getSystemService(PowerManager.class).isInteractive())
                .put("uiObservationUptimeMillis",SystemClock.uptimeMillis());
        }
        if(mode.equals("keyguard-diagnostic"))powerDiagnostic(context,state);
        if(mode.equals("keyguard-power"))state.put("powerDump",dump("power"));
        if(mode.equals("keyguard-await")) {
            long started=SystemClock.elapsedRealtime(),deadline=started+30000;int polls=1;
            while(!(state.getBoolean("secure") && !state.getBoolean("deviceLocked") && !state.getBoolean("keyguardLocked")
                    && state.getBoolean("interactive")) && SystemClock.elapsedRealtime()<deadline) {
                Thread.sleep(50);polls++;
                state.put("secure",keyguard.isDeviceSecure()).put("deviceLocked",keyguard.isDeviceLocked())
                    .put("keyguardLocked",keyguard.isKeyguardLocked()).put("interactive",context.getSystemService(PowerManager.class).isInteractive())
                    .put("uptimeMillis",SystemClock.uptimeMillis());
            }
            boolean unlocked=state.getBoolean("secure") && !state.getBoolean("deviceLocked") && !state.getBoolean("keyguardLocked") && state.getBoolean("interactive");
            state.put("unlockObserved",unlocked).put("keyguardObservationWaitMillis",SystemClock.elapsedRealtime()-started).put("keyguardObservationPolls",polls)
                .put("externalStorage",externalStorage(context,false));
            if(!unlocked)state.put("powerDump",dump("power"));
        }
        return state;
    }
    private static boolean keyguardOperation(String mode) {
        return mode.equals("keyguard") || mode.equals("keyguard-ui") || mode.equals("keyguard-diagnostic")
            || mode.equals("keyguard-power") || mode.equals("keyguard-await");
    }
    private static void keyguardObserver(Context context,String runId,String nonce)throws Exception {
        if(!runId.matches("[a-f0-9]{32}") || !nonce.matches("[a-f0-9]{32}"))throw new IOException("Invalid observer identity");
        // Load the real services before the host sends WAKEUP. UI inspection is
        // separately warmed before the gesture, never inferred from this read.
        keyguardState(context,"keyguard");
        System.out.println("POCKET_CE32_KEYGUARD_READY 1 "+runId+" "+nonce+" "+Os.getuid()+" "+Os.getpid());System.out.flush();
        BufferedReader requests=new BufferedReader(new InputStreamReader(System.in,"US-ASCII"));int sequence=0;
        for(String line;(line=requests.readLine())!=null;) {
            if(line.length()>160)throw new IOException("Observer request exceeds its bound");
            String[] fields=line.split(" ");
            if(fields.length!=5 || !fields[0].equals("1") || !fields[1].equals(runId) || !fields[2].equals(nonce)
                    || !fields[3].equals(Integer.toString(sequence+1)) || sequence>=256)throw new IOException("Invalid observer request identity/sequence");
            sequence++;String mode=fields[4],body;
            if(mode.equals("close"))body="closed";
            else {
                if(!keyguardOperation(mode))throw new IOException("Unsupported read-only observer request");
                org.json.JSONObject state=keyguardState(context,mode);
                state.put("observerRunId",runId).put("observerNonce",nonce).put("observerSequence",sequence)
                    .put("observerUid",Os.getuid()).put("observerPid",Os.getpid()).put("readOnly",true)
                    .put("passwordTextRead",false).put("inputInjected",false).put("observerOperation",mode);
                body=state.toString();
            }
            byte[] bytes=body.getBytes("UTF-8");if(bytes.length>65536)throw new IOException("Observer reply exceeds its bound");
            System.out.println("POCKET_CE32_KEYGUARD_RESPONSE 1 "+runId+" "+nonce+" "+sequence+" "+Os.getuid()+" "+Os.getpid()+" "+android.util.Base64.encodeToString(bytes,android.util.Base64.NO_WRAP));System.out.flush();
            if(mode.equals("close"))return;
        }
    }
    public static void main(String[] args) {
        try {
            if(Build.VERSION.SDK_INT!=32 || Os.getuid()!=0 || !(args.length==1 || args.length==3 && args[0].equals("keyguard-observer")))throw new IllegalStateException();
            Os.setgid(1000);Os.setuid(1000);Looper.prepare();
            Class<?> activity=Class.forName("android.app.ActivityThread");Object thread=activity.getMethod("systemMain").invoke(null);
            Context context=(Context)activity.getMethod("getSystemContext").invoke(thread);
            if(args[0].equals("keyguard-observer")){keyguardObserver(context,args[1],args[2]);System.exit(0);return;}
            if(args[0].equals("inventory")) {
                android.content.pm.PackageManager packages=context.getPackageManager();
                java.util.List<android.content.pm.PackageInfo> entries=packages.getInstalledPackages(
                    android.content.pm.PackageManager.GET_SIGNING_CERTIFICATES|android.content.pm.PackageManager.MATCH_UNINSTALLED_PACKAGES);
                java.util.Collections.sort(entries,(a,b)->a.packageName.compareTo(b.packageName));
                org.json.JSONArray inventory=new org.json.JSONArray();
                for(android.content.pm.PackageInfo entry:entries) {
                    java.util.List<String> certificates=new java.util.ArrayList<>();
                    if(entry.signingInfo!=null)for(android.content.pm.Signature signature:entry.signingInfo.getApkContentsSigners()) {
                        byte[] hash=java.security.MessageDigest.getInstance("SHA-256").digest(signature.toByteArray());StringBuilder text=new StringBuilder();
                        for(byte value:hash)text.append(String.format(java.util.Locale.ROOT,"%02x",value&255));certificates.add(text.toString());
                    }
                    java.util.Collections.sort(certificates);
                    inventory.put(new org.json.JSONObject().put("package",entry.packageName).put("uid",entry.applicationInfo.uid)
                        .put("versionCode",entry.getLongVersionCode()).put("installed",(entry.applicationInfo.flags&android.content.pm.ApplicationInfo.FLAG_INSTALLED)!=0)
                        .put("certificates",new org.json.JSONArray(certificates)));
                }
                System.out.println("POCKET_CE32_INVENTORY "+new org.json.JSONObject().put("packages",inventory).put("includesKeptData",true));
                System.out.flush();System.exit(0);return;
            }
            if(args[0].equals("keyguard") || args[0].equals("keyguard-ui") || args[0].equals("keyguard-diagnostic") || args[0].equals("keyguard-power") || args[0].equals("keyguard-await")) {
                org.json.JSONObject state=keyguardState(context,args[0]);
                System.out.println("POCKET_CE32_KEYGUARD "+state);System.out.flush();System.exit(0);return;
            }
            // Decrypting the filesystem precedes UserController's asynchronous
            // unlock/reconcile work. CE providers remain unavailable until that
            // work finishes, including on a boot with no lock-screen password.
            UserManager users=context.getSystemService(UserManager.class);
            long started=SystemClock.elapsedRealtime(),deadline=started+60000;
            while(!users.isUserUnlocked()) {
                if(SystemClock.elapsedRealtime()>=deadline)throw new IOException("Android user 0 did not finish unlocking before the CE provider check");
                Thread.sleep(100);
            }
            System.out.println("POCKET_CE32_USER_UNLOCKED wait_ms="+(SystemClock.elapsedRealtime()-started));
            String authority="dev.kreijstal.pocketce32test.state";Class<?> api=Class.forName("android.app.IActivityManager");
            Object manager=Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);IBinder token=new Binder();
            Object holder=api.getMethod("getContentProviderExternal",String.class,int.class,IBinder.class,String.class).invoke(manager,authority,0,token,"pocket-ce-test");
            if(holder==null)throw new IOException("Ordinary CE app provider unavailable");
            try {
                Object provider=holder.getClass().getField("provider").get(holder);
                try(ContentProviderClient client=(ContentProviderClient)ContentProviderClient.class.getConstructor(ContentResolver.class,
                    Class.forName("android.content.IContentProvider"),String.class,boolean.class).newInstance(context.getContentResolver(),provider,authority,true)) {
                    Bundle state=client.call(args[0],null,null);
                    if(state==null || state.getString("error")!=null || state.getString("result")==null)throw new IOException("CE app check failed: "+(state==null?"no result":state.getString("error")+" stage="+state.getString("errorStage")));
                    org.json.JSONObject observed=new org.json.JSONObject(state.getString("result"));
                    if(new File("/system/etc/pocket-external-volume32").isFile())observed.put("externalStorage",externalStorage(context,true,deadline));
                    System.out.println("POCKET_CE32_APP "+observed);
                }
            }finally{api.getMethod("removeContentProviderExternalAsUser",String.class,IBinder.class,int.class).invoke(manager,authority,token,0);}
            System.out.flush();System.exit(0);
        }catch(Throwable error){System.err.println("POCKET_CE32_APP_ERROR "+error);error.printStackTrace();System.exit(1);}
    }
}
