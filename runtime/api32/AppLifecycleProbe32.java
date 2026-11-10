// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;
import android.content.*;
import android.content.pm.*;
import android.os.*;
import android.system.Os;
import java.io.*;
import java.lang.reflect.*;
import java.util.UUID;
import java.util.concurrent.*;
import java.nio.file.*;
import org.json.JSONObject;

/** Real package sessions, ordinary app process/provider, and restart checks. */
public final class AppLifecycleProbe32 {
    private static final String PACKAGE="dev.kreijstal.pocketapi32guesttest";
    private static final String AUTHORITY=PACKAGE+".status";
    private static final String EVENTS="dev.kreijstal.aosppocket.guestbridge.events";
    private static void check(boolean condition,String message){if(!condition)throw new AssertionError(message);}
    private static final class StatusClient implements AutoCloseable {
        final Class<?> api;
        final Object manager;
        final IBinder token=new Binder();
        final String authority;
        final ContentProviderClient client;
        StatusClient(Context context)throws Exception {this(context,AUTHORITY);}
        StatusClient(Context context,String authority)throws Exception {
            this.authority=authority;
            api=Class.forName("android.app.IActivityManager");
            manager=Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
            Object holder=api.getMethod("getContentProviderExternal",String.class,int.class,IBinder.class,String.class)
                .invoke(manager,authority,0,token,"pocket-api32-app-test");
            check(holder!=null,"Installed status provider missing");
            Object provider=holder.getClass().getField("provider").get(holder);
            try {
                client=(ContentProviderClient)ContentProviderClient.class.getConstructor(ContentResolver.class,
                    Class.forName("android.content.IContentProvider"),String.class,boolean.class)
                    .newInstance(context.getContentResolver(),provider,authority,true);
            }catch(Exception error){try{release();}catch(Exception cleanup){error.addSuppressed(cleanup);}throw error;}
        }
        Bundle snapshot()throws Exception {return client.call("snapshot",null,null);}
        Bundle prepare(String pkg)throws Exception {return client.call("prepare",pkg,null);}
        Bundle trace()throws Exception {return client.call("trace",null,null);}
        private void release()throws Exception {
            api.getMethod("removeContentProviderExternalAsUser",String.class,IBinder.class,int.class)
                .invoke(manager,authority,token,0);
        }
        public void close()throws Exception {try{client.close();}finally{release();}}
    }
    private static void captureInputState() {
        for(String service:new String[]{"input","power","window"}) {
            FutureTask<Void> dump=new FutureTask<>(()->{
                IBinder binder=(IBinder)Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,service);
                if(binder==null)throw new IOException("Missing service "+service);
                File file=new File("/tmp/input32-"+service+"-state.txt");
                try(FileOutputStream output=new FileOutputStream(file)){binder.dump(output.getFD(),new String[0]);}
                byte[] bytes=Files.readAllBytes(file.toPath());
                System.out.println("API32_INPUT_STATE_BEGIN service="+service+" bytes="+bytes.length);
                System.out.println(new String(bytes,0,Math.min(bytes.length,131072),"UTF-8"));
                System.out.println("API32_INPUT_STATE_END service="+service);
                return null;
            });
            Thread worker=new Thread(dump,"Api32InputDump-"+service);worker.setDaemon(true);worker.start();
            try{dump.get(10,TimeUnit.SECONDS);}
            catch(Exception unavailable){System.err.println("API32_INPUT_STATE_UNAVAILABLE service="+service+" error="+unavailable);}
        }
    }
    private static void captureFailureStacks(StatusClient client) {
        try {
            Bundle state=client.trace();Thread.sleep(1000);
            System.out.println("API32_APP_FAILURE_TRACE_REQUESTED pid="+state.getInt("pid")+" uid="+state.getInt("uid")+" source=provider_self");
        }catch(Exception unavailable){System.err.println("API32_APP_FAILURE_TRACE_UNAVAILABLE "+unavailable);}
    }
    private static void awaitInstallEvent(Context context,PackageInfo info,StatusClient observer,long before,String action)throws Exception {
        long deadline=SystemClock.uptimeMillis()+90000;Bundle event=null;
        while(SystemClock.uptimeMillis()<deadline) {
            event=observer.snapshot();
            if(event!=null && event.getLong("sequence")>before && PACKAGE.equals(event.getString("eventPackage"))) {
                check(event.getString("error")==null,"Package event observer failed: "+event.getString("error"));
                if(action.equals(event.getString("eventAction")) && event.getLong("updateTime",-1)==info.lastUpdateTime) {
                    check(event.getInt("eventUid")==info.applicationInfo.uid && event.getLong("versionCode")==info.getLongVersionCode() &&
                        event.getInt("uid")>=10000,"Package event metadata mismatch");
                    System.out.println("API32_APP_INSTALL_READY_PASS action="+action+" observer=registered_app observer_uid="+event.getInt("uid")+
                        " update_time="+info.lastUpdateTime+" sequence_delta="+(event.getLong("sequence")-before));
                    return;
                }
            }
            Thread.sleep(200);
        }
        PackageInfo observed=context.getPackageManager().getPackageInfo(PACKAGE,PackageManager.GET_RECEIVERS);
        System.out.println("API32_APP_REPLACEMENT_STATE stopped="+((observed.applicationInfo.flags&ApplicationInfo.FLAG_STOPPED)!=0)+" update_time="+observed.lastUpdateTime);
        captureFailureStacks(observer);
        throw new AssertionError("Registered package observer did not finish: "+event);
    }
    private static void install(Context context,File apk)throws Exception {
        long previousUpdate=-1;
        try{previousUpdate=context.getPackageManager().getPackageInfo(PACKAGE,0).lastUpdateTime;}
        catch(PackageManager.NameNotFoundException absent){/* First install has no replacement/removal race. */}
        PackageInstaller installer=context.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params=new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(PACKAGE);params.setSize(apk.length());
        Field flags=params.getClass().getDeclaredField("installFlags");flags.setAccessible(true);flags.setInt(params,flags.getInt(params)|2);
        CountDownLatch delivered=new CountDownLatch(1);final Intent[] result={null};
        Binder receiver=new Binder(){
            @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException {
                if(code!=IBinder.FIRST_CALL_TRANSACTION)return super.onTransact(code,data,reply,flags);
                data.enforceInterface("android.content.IIntentSender");data.readInt();
                result[0]=data.readInt()!=0?Intent.CREATOR.createFromParcel(data):null;
                delivered.countDown();return true;
            }
        };
        receiver.attachInterface(null,"android.content.IIntentSender");
        Class<?> sender=Class.forName("android.content.IIntentSender");
        Object target=Class.forName("android.content.IIntentSender$Stub").getMethod("asInterface",IBinder.class).invoke(null,receiver);
        IntentSender callback=IntentSender.class.getConstructor(sender).newInstance(target);
        try(StatusClient observer=new StatusClient(context,EVENTS)) {
            Bundle armed=observer.prepare(PACKAGE);
            check(armed!=null && armed.getInt("uid")>=10000,"Package observer must be a registered ordinary application");
            long before=armed.getLong("sequence");
            int id=installer.createSession(params);
            try(PackageInstaller.Session session=installer.openSession(id)) {
                try(InputStream input=new FileInputStream(apk);OutputStream output=session.openWrite("base.apk",0,apk.length())) {
                    byte[] buffer=new byte[65536];int count;
                    while((count=input.read(buffer))!=-1)output.write(buffer,0,count);session.fsync(output);
                }
                session.commit(callback);
            }
            check(delivered.await(90,TimeUnit.SECONDS) && result[0]!=null,"PackageInstaller callback missing");
            int status=result[0].getIntExtra(PackageInstaller.EXTRA_STATUS,-999);
            check(status==PackageInstaller.STATUS_SUCCESS,"Install status="+status+" message="+result[0].getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
            PackageInfo installed=context.getPackageManager().getPackageInfo(PACKAGE,0);
            ApplicationInfo info=installed.applicationInfo;
            check(info.uid>=10000 && info.minSdkVersion==32 && info.targetSdkVersion==32,"Installed package metadata mismatch");
            check(installed.lastUpdateTime>0 && installed.lastUpdateTime!=previousUpdate,"Install update timestamp was not advanced");
            awaitInstallEvent(context,installed,observer,before,previousUpdate>=0?Intent.ACTION_PACKAGE_REPLACED:Intent.ACTION_PACKAGE_ADDED);
            System.out.println("API32_APP_INSTALL_PASS package="+PACKAGE+" uid="+info.uid+" min_sdk=32 target_sdk=32 session="+id);
        }
    }
    private static void launch(String nonce)throws Exception {
        Class<?> type=Class.forName("android.app.IActivityTaskManager");
        Object manager=Class.forName("android.app.ActivityTaskManager").getMethod("getService").invoke(null);
        Method method=null;for(Method candidate:type.getMethods())if(candidate.getName().equals("startActivityAsUser") &&
            candidate.getParameterCount()==12 && candidate.getParameterTypes()[3]==Intent.class)method=candidate;
        check(method!=null,"12L activity service ABI missing");
        Intent intent=new Intent(Intent.ACTION_MAIN).setComponent(new ComponentName(PACKAGE,PACKAGE+".MainActivity"))
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK|Intent.FLAG_ACTIVITY_CLEAR_TASK).putExtra("nonce",nonce);
        int status=(Integer)method.invoke(manager,null,"android",null,intent,null,null,null,0,0,null,null,0);
        check(status>=0,"Activity start failed "+status);
    }
    private static Bundle appState(Context context,String nonce)throws Exception {
        return appState(context,nonce,false);
    }
    private static Bundle appState(Context context,String nonce,boolean awaitTap)throws Exception {
        try(StatusClient client=new StatusClient(context)) {
                long deadline=SystemClock.uptimeMillis()+90000;Bundle state=null;boolean announced=false;String lastReady=null;
                while(SystemClock.uptimeMillis()<deadline) {
                    state=client.snapshot();
                    if(state!=null && nonce.equals(state.getString("nonce")) && state.getBoolean("resumed") && state.getBoolean("checksDone")) {
                        if(!awaitTap)return state;
                        if(state.getBoolean("drawn") && state.getInt("patternWidth")>=16 && state.getInt("patternHeight")>0) {
                            JSONObject ready=new JSONObject().put("runId",System.getenv("POCKET_API32_RUN_ID")).put("nonce",nonce);
                            String target=nonce;
                            for(String key:new String[]{"patternLeft","patternTop","patternWidth","patternHeight","buttonX","buttonY","editorX","editorY"}) {
                                int value=state.getInt(key);ready.put(key,value);target+=":"+value;
                            }
                            int clicks=state.getInt("clicks");check(clicks<=3,"Host taps duplicated: "+clicks);
                            ready.put("clicks",clicks).put("expectedClicks",3).put("textFocused",state.getBoolean("textFocused"))
                                .put("typedText",state.getString("typedText","")).put("keyUps",state.getInt("keyUps"));
                            ready.put("targetToken",target);String encoded=ready.toString();
                            // SystemUI can publish its insets after the first
                            // app draw. Keep the observed layout current, and
                            // require host pixels for this exact geometry.
                            if(!encoded.equals(lastReady)) {
                                Path file=Paths.get("/tmp/app32-input-ready.json"),temporary=Paths.get("/tmp/app32-input-ready.json.new");
                                Files.write(temporary,encoded.getBytes("UTF-8"));Files.move(temporary,file,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
                                System.out.println("API32_APP_INPUT_READY nonce="+nonce+" target="+target);lastReady=encoded;announced=true;
                            }
                        }
                        if(announced && state.getInt("clicks")==3 && state.getBoolean("textFocused") &&
                            "a129".equals(state.getString("typedText")) && state.getInt("keyUps")>=6 &&
                            state.getInt("lastKeyUp")==android.view.KeyEvent.KEYCODE_9)return state;
                    }
                    if(awaitTap && announced && (state==null || !state.getBoolean("resumed"))) {
                        Files.deleteIfExists(Paths.get("/tmp/app32-input-ready.json"));lastReady=null;announced=false;
                    }
                    Thread.sleep(200);
                }
                if(awaitTap)captureInputState();
                captureFailureStacks(client);
                throw new AssertionError("Installed activity did not finish checks: "+state);
        }
    }
    private static Bundle validate(Context context,String nonce)throws Exception {
        launch(nonce);Bundle result=appState(context,nonce);
        check(result.getString("error")==null,"Installed app check failed: "+result.getString("error"));
        int uid=context.getPackageManager().getApplicationInfo(PACKAGE,0).uid;
        check(result.getInt("uid")==uid && result.getInt("providerUid")==uid && uid>=10000,"Ordinary app UID mismatch");
        check(result.getInt("pid")>0 && result.getInt("pid")==result.getInt("providerPid"),"Activity/provider process mismatch");
        check(result.getBoolean("settings") && result.getBoolean("keystore") && result.getInt("httpStatus")==200 && result.getInt("httpBytes")>0,
            "Installed app operations incomplete: "+result);
        System.out.println("API32_APP_ACTIVITY_PASS uid="+uid+" pid="+result.getInt("pid")+" resumed=1 drawn="+result.getBoolean("drawn"));
        return result;
    }
    public static void main(String[] args) {
        try {
            check(Build.VERSION.SDK_INT==32 && Os.getuid()==0 && args.length==1,"Expected SDK 32 root entry and APK");
            Os.setgid(1000);Os.setuid(1000);Looper.prepare();
            Class<?> thread=Class.forName("android.app.ActivityThread");Object main=thread.getMethod("systemMain").invoke(null);
            Context context=(Context)thread.getMethod("getSystemContext").invoke(main);
            install(context,new File(args[0]));
            Bundle first=validate(context,UUID.randomUUID().toString().replace("-",""));
            Object manager=Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
            Class.forName("android.app.IActivityManager").getMethod("forceStopPackage",String.class,int.class).invoke(manager,PACKAGE,0);
            Bundle second=validate(context,UUID.randomUUID().toString().replace("-",""));
            check(second.getInt("pid")!=first.getInt("pid") && second.getLong("processStarts")==first.getLong("processStarts")+1 &&
                first.getString("digest")!=null && first.getString("digest").equals(second.getString("digest")),"App restart/private state mismatch");
            System.out.println("API32_APP_SETTINGS_PASS client=ordinary_app read=real query=real");
            System.out.println("API32_APP_KEYSTORE_PASS provider=AndroidKeyStore key_export=denied private_ciphertext=verified process_restart=1");
            System.out.println("API32_APP_HTTPS_PASS uid="+second.getInt("uid")+" status=200 bytes="+second.getInt("httpBytes")+" default_tls_verification=1");
            System.out.println("API32_APP_RESTART_PASS force_stop=AMS pid_changed=1 private_state=persisted launches_incremented=1 guest_restart=unverified");
            if("1".equals(System.getenv("POCKET_API32_HOST_DISPLAY"))) {
                try(android.net.LocalSocket peer=new android.net.LocalSocket()) {
                    peer.connect(new android.net.LocalSocketAddress("/dev/socket/pocket_control32",android.net.LocalSocketAddress.Namespace.FILESYSTEM));
                    peer.setSoTimeout(3000);
                    try{new DataInputStream(peer.getInputStream()).readInt();throw new AssertionError("Guest process accepted on host-only input channel");}
                    catch(EOFException denied){System.out.println("API32_INPUT_GUEST_PEER_DENIED_PASS uid=1000 kernel_pid=not_host_owner");}
                }
                Bundle tapped=appState(context,second.getString("nonce"),true);
                check(tapped.getInt("clicks")==3,"Host tap was lost or duplicated");
                System.out.println("API32_HOST_INPUT_PASS ordinary_app_clicks=3 nonce="+second.getString("nonce"));
                System.out.println("API32_HOST_KEYBOARD_PASS text=a129 key_ups="+tapped.getInt("keyUps")+" stock_EditText=1 deletion=real nonce="+second.getString("nonce"));
            }
            System.exit(0);
        } catch(Throwable error){error.printStackTrace();System.err.println("API32_APP_LIFECYCLE_FAILED");System.exit(1);}
    }
}
