// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import android.content.Intent;
import android.net.Uri;
import android.os.*;
import java.io.*;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.concurrent.*;
import org.json.*;
import dev.kreijstal.aosppocket.storage.CeNamespace;

/** Full Manager/12L credential and encrypted-app lifecycle in a separate profile. */
public final class ManagerCeProbe32 extends ManagerSessionProbe32 {
    private volatile JSONObject pendingCycle;
    private volatile int acknowledged=-1;
    private String canaryRunId;
    private File initBackup;
    private int initMode;
    private String originalInitSha;
    private JSONObject initTrial;
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        if(getIntent().getBooleanExtra("ce_authless_verify",false))autoStart=false;
    }
    private static String hash(File file)throws Exception {
        MessageDigest digest=MessageDigest.getInstance("SHA-256");
        try(InputStream in=new FileInputStream(file)){byte[] bytes=new byte[65536];int size;while((size=in.read(bytes))!=-1)digest.update(bytes,0,size);}
        StringBuilder text=new StringBuilder();for(byte b:digest.digest())text.append(String.format(Locale.ROOT,"%02x",b&255));return text.toString();
    }
    private void patchInitTrial()throws Exception {
        JSONObject spec;
        try(InputStream in=getAssets().open("api32-ce-init.json")) {
            ByteArrayOutputStream bytes=new ByteArrayOutputStream();GuestImage.copy(in,bytes,8192);spec=new JSONObject(bytes.toString("UTF-8"));
        }catch(FileNotFoundException absent){return;}
        check(DirectImage.leftovers(root).isEmpty(),"Cannot replace an active guest init");
        File[] prior=root.listFiles();check(prior!=null,"Cannot inspect prior init trials");
        for(File file:prior)if(file.getName().startsWith(".pocket-ce-init-"))check(new File(file,"restored").isFile(),"Inspect the unfinished native-init trial first");
        File target=new File(root,"system/bin/pocket-init-api32"),backup=new File(root,".pocket-ce-init-"+runId);
        check(target.isFile() && !Files.isSymbolicLink(target.toPath()) && !backup.exists() && backup.mkdir(),"Unexpected native-init trial path");
        initMode=android.system.Os.stat(target.getPath()).st_mode&07777;originalInitSha=hash(target);
        File saved=new File(backup,"pocket-init-api32");Files.copy(target.toPath(),saved.toPath());
        check(hash(saved).equals(originalInitSha),"Native-init backup differs");initBackup=saved;
        initTrial=new JSONObject().put("assetSha256",spec.getString("assetSha256")).put("sourceSha256",spec.getString("sourceSha256"))
            .put("originalSha256",originalInitSha).put("originalMode",initMode).put("backup",backup.getName())
            .put("guestDataReset",false).put("runtimeCodeReplaced",true).put("originalCodeRestored",false);
        result.put("initTrial",initTrial);Files.write(new File(backup,"trial.json").toPath(),initTrial.toString().getBytes("UTF-8"));
        File temporary=new File(target.getPath()+".trial-"+runId);
        try {
            try(InputStream in=getAssets().open("api32-ce-init");OutputStream out=new FileOutputStream(temporary)){GuestImage.copy(in,out,4*1024*1024);}
            check(hash(temporary).equals(spec.getString("assetSha256")),"Native-init trial payload differs");
            java.lang.reflect.Method patch=DirectImage.class.getDeclaredMethod("patchInterpreter",File.class,String.class);patch.setAccessible(true);
            patch.invoke(null,temporary,new File(root,"system/bin/pocket-loader").getPath());android.system.Os.chmod(temporary.getPath(),initMode);
            Files.move(temporary.toPath(),target.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
        }finally{Files.deleteIfExists(temporary.toPath());}
    }
    private void restoreInitTrial()throws Exception {
        if(initBackup==null)return;
        check(DirectImage.leftovers(root).isEmpty(),"Cannot restore native init while guest processes remain");
        check(hash(initBackup).equals(originalInitSha),"Native-init backup changed");
        File target=new File(root,"system/bin/pocket-init-api32"),temporary=new File(target.getPath()+".restore-"+runId);
        Files.copy(initBackup.toPath(),temporary.toPath());android.system.Os.chmod(temporary.getPath(),initMode);
        Files.move(temporary.toPath(),target.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
        check(hash(target).equals(originalInitSha),"Native-init restoration differs");
        initTrial.put("originalCodeRestored",true).put("restoredSha256",hash(target));
        Files.write(new File(initBackup.getParentFile(),"restored").toPath(),runId.getBytes("US-ASCII"));initBackup=null;
    }
    private final String[] passwords={password(),password(),password()};
    private static String password(){return "P32"+UUID.randomUUID().toString().replace("-","").substring(0,14);}
    private String redact(String text){for(String value:passwords)text=text.replace(value,"<redacted>");return text;}
    private void loadTestPasswords()throws Exception {
        java.net.URL url=new java.net.URL(getIntent().getStringExtra("ce_secret_url"));
        check(url.getProtocol().equals("http") && url.getHost().equals("127.0.0.1") && url.getPort()==8795
            && url.getPath().equals("/"+runId) && url.getUserInfo()==null && url.getQuery()==null,"Expected isolated test credential channel");
        java.net.HttpURLConnection connection=(java.net.HttpURLConnection)url.openConnection();
        connection.setConnectTimeout(10000);connection.setReadTimeout(10000);connection.setInstanceFollowRedirects(false);
        try {
            check(connection.getResponseCode()==200,"Test credential channel unavailable");
            ByteArrayOutputStream bytes=new ByteArrayOutputStream();
            try(InputStream in=connection.getInputStream()){GuestImage.copy(in,bytes,4096);}
            JSONObject value=new JSONObject(bytes.toString("UTF-8"));
            check(value.getString("runId").equals(runId),"Wrong test credential nonce");
            JSONArray entries=value.getJSONArray("passwords");check(entries.length()==passwords.length,"Wrong test credential count");
            String[] received=new String[passwords.length];
            for(int i=0;i<received.length;i++){received[i]=entries.getString(i);check(received[i].matches("P32[a-f0-9]{14}"),"Invalid test credential format");}
            System.arraycopy(received,0,passwords,0,passwords.length);
        }finally{connection.disconnect();}
    }
    @Override public void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if(runId.equals(intent.getStringExtra("probe_run_id")) && intent.hasExtra("ce_cycle_ack"))acknowledged=intent.getIntExtra("ce_cycle_ack",-1);
    }
    @Override public void dump(String prefix,FileDescriptor fd,PrintWriter out,String[] args) {
        if(args!=null && Arrays.asList(args).contains("pocket-ce-keyguard-image")) {
            File image=new File(getFilesDir(),"ce32-keyguard-bouncer-"+runId+".png");
            if(!image.isFile())image=new File(getFilesDir(),"ce32-keyguard-before-"+runId+".png");
            try {
                check(image.isFile() && image.length()<=1024*1024,"No bounded keyguard image available");
                out.println("POCKET_CE_FRAME32="+android.util.Base64.encodeToString(Files.readAllBytes(image.toPath()),android.util.Base64.NO_WRAP));
            }catch(Exception error){out.println("POCKET_CE_FRAME32_ERROR="+error.getClass().getSimpleName());}
            return;
        }
        boolean cycle=false;if(args!=null)for(String arg:args)if(arg.equals("pocket-ce-cycle"))cycle=true;
        if(cycle) {
            String text=pendingCycle==null?"{}":pendingCycle.toString();
            out.println("POCKET_CE_CYCLE="+android.util.Base64.encodeToString(text.getBytes(java.nio.charset.StandardCharsets.UTF_8),android.util.Base64.NO_WRAP));
        }else super.dump(prefix,fd,out,args);
    }
    private JSONObject command(List<String> argv,String input,int timeout)throws Exception {
        ProcessBuilder builder=new ProcessBuilder(SyscallBackend.command(root,"/",argv)).directory(root);
        builder.environment().clear();builder.environment().putAll(SyscallBackend.environment(root,android.os.Process.myPid(),32));
        File log=new File(root,"logs/ce-command-"+runId+".log");builder.redirectErrorStream(true).redirectOutput(log);java.lang.Process process=builder.start();
        try {
            try(OutputStream out=process.getOutputStream()){if(input!=null)out.write(input.getBytes("UTF-8"));}
            check(process.waitFor(timeout,TimeUnit.SECONDS),"CE command timed out");
            check(log.length()<65536,"CE command output exceeds limit");
            return new JSONObject().put("exit",process.exitValue()).put("output",redact(new String(Files.readAllBytes(log.toPath()),"UTF-8")));
        }finally{if(process.isAlive())process.destroyForcibly();}
    }
    private String javaShell(String classpath,String type,String argument) {
        return ". /system/etc/pocket-runtime-env.sh; export LD_PRELOAD=/system/lib64/libpocket_binder_api32.so; exec /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp "+classpath+" /system/bin "+type+" "+argument;
    }
    private JSONObject credential(String mode,String input,boolean rejected)throws Exception {
        JSONObject response=command(Arrays.asList("/system/bin/sh","-c",javaShell("/system/framework/pocket-manager.jar","dev.kreijstal.aosppocket.guest.GuestCredential32",mode)),input,135);
        String text=response.getString("output");
        if(rejected) {check(response.getInt("exit")==2 && text.contains("POCKET_CREDENTIAL32_ERROR SecurityException"),"Wrong password was not rejected by LockSettings: "+text);return new JSONObject().put("rejected",true);}
        check(response.getInt("exit")==0,"Credential operation failed: "+text);
        String tag="POCKET_CREDENTIAL32_RESULT ";for(String line:text.split("\n"))if(line.startsWith(tag))return new JSONObject(line.substring(tag.length()));
        throw new IOException("Credential helper returned no result");
    }
    private JSONObject app(String operation)throws Exception {
        JSONObject response=command(Arrays.asList("/system/bin/sh","-c",javaShell("/tmp/manager-ce-observer.apk","dev.kreijstal.aosppocket.guest.CeAppInspector32",operation)),null,90);
        String text=response.getString("output");check(response.getInt("exit")==0,"Ordinary CE app failed: "+text);
        java.util.regex.Matcher unlocked=java.util.regex.Pattern.compile("(?m)^POCKET_CE32_USER_UNLOCKED wait_ms=(\\d+)$").matcher(text);
        check(unlocked.find(),"CE provider inspector did not observe Android user unlock");long unlockWait=Long.parseLong(unlocked.group(1));
        String tag="POCKET_CE32_APP ";for(String line:text.split("\n"))if(line.startsWith(tag)) {
            JSONObject value=new JSONObject(line.substring(tag.length()));check(value.getInt("sdk")==32 && value.getInt("uid")>=10000 && value.getBoolean("nonExportable") && value.getBoolean("cryptoValid"),"Invalid ordinary CE app result");return value.put("androidUserUnlocked",true).put("androidUnlockWaitMs",unlockWait);
        }
        throw new IOException("Ordinary app observer returned no result");
    }
    private JSONObject keyguard()throws Exception{return keyguard(false);}
    private JSONObject keyguard(boolean inspectUi)throws Exception {
        return keyguardMode(inspectUi?"keyguard-ui":"keyguard");
    }
    private JSONObject keyguardMode(String mode)throws Exception {
        JSONObject response=command(Arrays.asList("/system/bin/sh","-c",javaShell("/tmp/manager-ce-observer.apk","dev.kreijstal.aosppocket.guest.CeAppInspector32",mode)),null,45);
        String output=redact(response.getString("output"));
        check(response.getInt("exit")==0,"Keyguard observation failed: "+output.substring(Math.max(0,output.length()-3000)));
        String tag="POCKET_CE32_KEYGUARD ";
        for(String line:response.getString("output").split("\n"))if(line.startsWith(tag))return new JSONObject(line.substring(tag.length()));
        throw new IOException("Keyguard observer returned no state");
    }
    private void keyguardSwipe()throws Exception {
        long down=SystemClock.uptimeMillis();
        for(int step=0;step<=12;step++) {
            final int index=step;
            ui(()->{
                int action=index==0?android.view.MotionEvent.ACTION_DOWN:index==12?android.view.MotionEvent.ACTION_UP:android.view.MotionEvent.ACTION_MOVE;
                android.view.MotionEvent event=android.view.MotionEvent.obtain(down,SystemClock.uptimeMillis(),action,runtime.width*.5f,runtime.height*(.86f-.62f*index/12),0);
                try{runtime.touch(event,1,1);}finally{event.recycle();}return null;
            });Thread.sleep(40);
        }
    }
    private void keyguardTap(int x,int y)throws Exception {
        check(x>=0 && y>=0 && x<runtime.width && y<runtime.height,"Password-entry bounds outside the guest display");
        long down=SystemClock.uptimeMillis();
        for(int action:new int[]{android.view.MotionEvent.ACTION_DOWN,android.view.MotionEvent.ACTION_UP}) {
            ui(()->{
                android.view.MotionEvent event=android.view.MotionEvent.obtain(down,SystemClock.uptimeMillis(),action,x,y,0);
                try{runtime.touch(event,1,1);}finally{event.recycle();}return null;
            });Thread.sleep(60);
        }
    }
    private static boolean passwordEntryReady(JSONObject state,boolean focus) {
        JSONObject observer=state.optJSONObject("ui"),entry=observer==null?null:observer.optJSONObject("passwordEntry");
        return state.optBoolean("interactive") && observer!=null && observer.optBoolean("readOnly") && !observer.optBoolean("passwordTextRead",true)
            && entry!=null && entry.optString("resourceId").equals("com.android.systemui:id/passwordEntry")
            && entry.optBoolean("visible") && entry.optBoolean("enabled") && entry.optBoolean("editable") && entry.optBoolean("password")
            && (!focus || entry.optBoolean("focused"));
    }
    private JSONObject wakeKeyguard(String purpose)throws Exception {
        long deadline=SystemClock.elapsedRealtime()+15000;JSONObject state;
        JSONArray observations=result.optJSONArray("keyguardWakeAttempts");
        if(observations==null){observations=new JSONArray();result.put("keyguardWakeAttempts",observations);}
        do {
            long sent=SystemClock.uptimeMillis();
            if(!result.has("hostWakeUptimeMillis"))result.put("hostWakeUptimeMillis",sent);
            ui(()->{runtime.key(android.view.KeyEvent.KEYCODE_WAKEUP);return null;});
            state=keyguard();
            observations.put(new JSONObject().put("purpose",purpose).put("hostUptimeMillis",sent).put("state",state));
            stage("observing-keyguard-wake");
            if(state.optBoolean("interactive"))return state;
            // WAKEUP is a no-op while already awake. If Android's timeout
            // expires while the observer starts, send a new event after the
            // observed sleep instead of only polling the sleeping display.
            Thread.sleep(100);
        }while(SystemClock.elapsedRealtime()<deadline);
        result.put("keyguardBeforeInput",state).put("keyguardWakeDiagnostic",keyguardMode("keyguard-diagnostic"));
        throw new IOException("Guest display did not wake before "+purpose);
    }
    private JSONObject unlockWithKeyguard(String password)throws Exception {
        stage("waking-keyguard-before-frame");
        JSONObject before=wakeKeyguard("the first keyguard frame");
        result.put("keyguardBeforeInput",before);
        frame("ce32-keyguard-before-"+runId+".png");
        check(before.getBoolean("secure") && before.getBoolean("deviceLocked") && before.getBoolean("keyguardLocked"),"Secure keyguard was not locked: "+before);
        stage("unlocking-keyguard-ui");JSONObject ready=null;JSONArray attempts=new JSONArray();
        for(int attempt=0;attempt<3;attempt++) {
            wakeKeyguard("the keyguard gesture");
            keyguardSwipe();Thread.sleep(500);JSONObject state=keyguard(true);attempts.put(state);
            result.put("keyguardUiAttempts",attempts);stage("observing-keyguard-password-entry");
            if(!passwordEntryReady(state,false))continue;
            if(!passwordEntryReady(state,true)) {
                JSONArray bounds=state.getJSONObject("ui").getJSONObject("passwordEntry").getJSONArray("bounds");
                keyguardTap((bounds.getInt(0)+bounds.getInt(2))/2,(bounds.getInt(1)+bounds.getInt(3))/2);
                Thread.sleep(200);state=keyguard(true);attempts.put(state);
            }
            if(passwordEntryReady(state,true)){ready=state;break;}
        }
        check(ready!=null,"Password entry was not visibly focused; no password keys were sent");
        frame("ce32-keyguard-bouncer-"+runId+".png");
        result.put("keyguardUiBeforeEntry",ready).put("hostPasswordInputUptimeMillis",SystemClock.uptimeMillis());
        stage("typing-keyguard-password");
        android.view.KeyEvent[] keys=android.view.KeyCharacterMap.load(android.view.KeyCharacterMap.VIRTUAL_KEYBOARD).getEvents(password.toCharArray());
        check(keys!=null && keys.length>0,"Cannot encode keyguard password as key events");
        result.put("hostPasswordEncodedUptimeMillis",SystemClock.uptimeMillis())
            .put("firstPasswordEventUptimeMillis",keys[0].getEventTime())
            .put("lastPasswordEventUptimeMillis",keys[keys.length-1].getEventTime());
        for(android.view.KeyEvent event:keys) {
            check(ui(()->runtime.fullKey(event)),"Keyguard key event was not queued");Thread.sleep(15);
        }
        ui(()->{runtime.key(android.view.KeyEvent.KEYCODE_ENTER);return null;});
        result.put("hostPasswordSubmittedUptimeMillis",SystemClock.uptimeMillis());
        // Keep lock/power evidence even if the subsequent CE read times out.
        // Never dump input history after password keys have been sent.
        // Observe the actual transition in one process. Starting separate ART
        // observers around CE reads can span the normal 10s keyguard timeout.
        JSONObject after=keyguardMode("keyguard-await");result.put("keyguardAfterInput",after);stage("waiting-keyguard-ce-unlock");
        check(after.getBoolean("secure") && !after.getBoolean("deviceLocked") && !after.getBoolean("keyguardLocked")
            && after.getBoolean("interactive") && after.getBoolean("unlockObserved"),"Keyguard did not unlock after physical input: "+after);
        awaitCanaries();
        JSONObject input=session(false).getJSONObject("input");
        check(input.getLong("keyEventsAccepted")>=keys.length+2 && input.getLong("touchEventsAccepted")>=2 && input.getLong("inputEventsRejected")==0,"Keyguard input transport failed");
        return new JSONObject().put("before",before).put("after",after).put("physicalInput",true).put("cliCorrectPasswordUsed",false).put("input",input)
            .put("passwordEntryVerified",true).put("uiBeforeEntry",ready).put("uiAttempts",attempts)
            .put("wakeAttempts",result.getJSONArray("keyguardWakeAttempts"));
    }
    private void install()throws Exception {
        File apk=new File(getFilesDir(),"ce32-test.apk");
        try(InputStream in=getAssets().open("api32-ce-test.apk");OutputStream out=new FileOutputStream(apk)){GuestImage.copy(in,out,1024*1024);}
        ui(()->{runtime.installApps(Collections.singletonMap("base.apk",Uri.fromFile(apk)));return null;});
        long until=SystemClock.elapsedRealtime()+180000;
        while(true) {
            String message=ui(()->runtime.status());if(message.equals("Installed dev.kreijstal.pocketce32test"))return;
            check(!message.contains("operation failed") && SystemClock.elapsedRealtime()<until,"CE app install: "+message);Thread.sleep(250);
        }
    }
    private void canaries(boolean readable)throws Exception {
        String expected="ce32-canary-"+(canaryRunId==null?runId:canaryRunId);
        for(String base:CeNamespace.API32_ROOTS) {
            JSONObject response=command(Arrays.asList("/system/bin/toybox","cat",base+"/.pocket-ce32-canary"),null,20);
            if(readable)check(response.getInt("exit")==0 && response.getString("output").equals(expected),"CE canary missing or changed: "+base);
            else check(response.getInt("exit")!=0 && !response.getString("output").contains(expected),"CE root was readable before unlock: "+base);
        }
    }
    private boolean publicMediaCanary()throws Exception {
        if(!ExternalVolume32.selected(root))return false;
        JSONObject visible=command(Arrays.asList("/system/bin/toybox","cat","/storage/emulated/0/.pocket-ce32-canary"),null,20);
        check(visible.getInt("exit")==0 && visible.getString("output").equals("ce32-canary-"+(canaryRunId==null?runId:canaryRunId)),"Public emulated CE canary differs");
        return true;
    }
    private void awaitCanaries()throws Exception {
        long until=SystemClock.elapsedRealtime()+30000;
        while(true) {
            JSONObject response=command(Arrays.asList("/system/bin/toybox","cat","/data/user/0/.pocket-ce32-canary"),null,20);
            if(response.getInt("exit")==0){check(response.getString("output").equals("ce32-canary-"+(canaryRunId==null?runId:canaryRunId)),"Wrong decrypted bytes");break;}
            check(SystemClock.elapsedRealtime()<until,"CE did not unlock after credential verification");Thread.sleep(100);
        }
        canaries(true);
    }
    private JSONObject appReadiness(boolean locked)throws Exception {
        if(!new File(root,"system/etc/pocket-app-readiness32").isFile())return null;
        check(GuestManager32.selected(root),"Credential readiness requires owner control");
        int width=ui(()->runtime.width),height=ui(()->runtime.height);
        long deadline=SystemClock.elapsedRealtime()+180000;JSONObject state;
        do {
            state=new JSONObject(GuestManager32.execute(root,width,height,ConcurrentHashMap.newKeySet(),()->false,
                new File(root,"logs/ce-startup-readiness.log"),new String[]{"startup"}));
            check(state.getInt("sdk")==32 && state.getInt("uid")==1000
                && state.getString("source").equals("LocalServices.UserController.mBootCompleted"),"Wrong credential readiness observer");
            if(locked) {
                check(!state.getBoolean("userUnlocked") && !state.getBoolean("readyForApps"),"Secure boot unexpectedly unlocked");
                if(state.getBoolean("credentialRequired"))break;
            }else if(state.getBoolean("readyForApps")) {
                check(state.getBoolean("bootBroadcastComplete") && state.getBoolean("userUnlocked")
                    && state.getBoolean("primaryVolumeMounted") && !state.getBoolean("credentialRequired"),"Incomplete credential app readiness");
                return state;
            }
            check(SystemClock.elapsedRealtime()<deadline,"Credential app readiness timed out");Thread.sleep(250);
        }while(true);
        long start=SystemClock.uptimeMillis();boolean rejected=false;
        try {
            GuestManager32.execute(root,width,height,ConcurrentHashMap.newKeySet(),()->false,
                new File(root,"logs/ce-locked-open.log"),new String[]{"open","com.android.settings"});
        }catch(IOException error){rejected=error.getMessage()!=null && error.getMessage().contains("Unlock Android before using apps");}
        check(rejected,"Locked owner app operation was not rejected for credentials");
        return state.put("lockedAppOperationRejected",true).put("openRejectionMillis",SystemClock.uptimeMillis()-start);
    }
    private void capture(int index,boolean migrating)throws Exception {
        if(migrating) {
            ui(()->{runtime.stopGuest();return null;});long until=SystemClock.elapsedRealtime()+180000;
            while(ui(()->runtime.running() || runtime.starting() || runtime.stopping())) {
                check(SystemClock.elapsedRealtime()<until,"Verified CE migration did not finish");Thread.sleep(100);
            }
            check(DirectImage.leftovers(root).isEmpty(),"Migration left guest processes");
            check(CeStorage.enabled(root) && new File(root,CeMigration.LAYOUT32).isFile(),"Migration did not enable the five-root encrypted layout");
            CeMigration.sealPlaceholders(root);
        }else stop();
        restoreInitTrial();
        JSONObject state=session(true);String nativeText=redact(state.optString("nativeLog"));state.remove("nativeLog");
        check(state.optInt("ownerExitCode",-1)==0 && !state.optBoolean("ownerAlive",true) && !state.has("cleanupFailure"),"Guest owner did not exit cleanly");
        check(nativeText.length()<2*1024*1024,"Native log reached capture limit");
        JSONObject displayState=new JSONObject(new String(Files.readAllBytes(new File(root,"logs/display-performance.json").toPath()),"UTF-8"));
        check(displayState.getLong("frames_presented")>0,"No host frames presented");
        pendingCycle=new JSONObject().put("runId",runId).put("index",index).put("session",state).put("nativeLog",nativeText)
            .put("console",redact(console())).put("display",displayState).put("migration",migrating);
        result.put("cycleIndex",index);stage("cycle-stopped");long until=SystemClock.elapsedRealtime()+120000;
        while(acknowledged!=index){check(SystemClock.elapsedRealtime()<until,"Host did not validate the stopped cycle");Thread.sleep(100);}
        pendingCycle=null;
    }
    private void outerRestart()throws Exception {
        File file=new File(getFilesDir(),"ce32-restart-state.json");
        check(file.isFile() && file.length()<65536,"Missing bounded restart state");
        JSONObject saved=new JSONObject(new String(Files.readAllBytes(file.toPath()),"UTF-8"));
        check(saved.getString("runId").equals(runId) && saved.getInt("verifiedBoots")==5
            && saved.getInt("hostPid")!=android.os.Process.myPid(),"Wrong outer restart owner");
        if(saved.optBoolean("preservedLifecycle")) {
            canaryRunId=saved.getString("canaryRunId");
            check(canaryRunId.matches("[a-f0-9]{32}") && canaryRunId.equals(getIntent().getStringExtra("ce_preserved_nonce")),"Wrong preserved outer-restart profile");
            result.put("preservedLifecycle",true).put("preservedRunId",canaryRunId);
        }
        check(CeStorage.enabled(root) && new File(root,CeMigration.LAYOUT32).isFile(),"Encrypted layout missing after outer restart");
        result.put("outerRestart",true).put("previousHostPid",saved.getInt("hostPid"));stage("booting-after-outer-restart");boot();
        checkCrashTransport();canaries(false);credential("check",passwords[1]+"\n",true);canaries(false);
        result.put("lockedAppReadiness",appReadiness(true));
        JSONObject keyguard=unlockWithKeyguard(passwords[2]),original=saved.getJSONObject("app");
        result.put("unlockedAppReadiness",appReadiness(false));JSONObject current=app("verify");
        check(current.getString("token").equals(original.getString("token")) && current.getString("cipherDigest").equals(original.getString("cipherDigest"))
            && current.getInt("uid")==original.getInt("uid") && current.getInt("pid")!=original.getInt("pid"),"App data/keystore changed across the outer restart");
        result.put("keyguardUi",keyguard).put("app",current).put("ordinaryAppPersisted",true);
        canaries(true);result.put("readableCanariesVerified",5).put("unlockedExternalCanaryVerified",publicMediaCanary());
        if(saved.optBoolean("preservedLifecycle")) {
            stage("restoring-original-password-free-state");credential("set",passwords[2]+"\n\n",false);
            check(new File(root,".pocket-ce-authless").isFile() && !keyguard().getBoolean("secure"),"Original password-free state was not restored");
            result.put("originalPasswordFreeStateRestored",true);
        }
        capture(5,false);result.put("validation","PASS").put("scope","Encrypted five-root storage, real keyguard input and ordinary app/keystore persistence after outer Manager process restart");
    }
    private void verifyPreservedAuthless()throws Exception {
        canaryRunId=getIntent().getStringExtra("ce_preserved_nonce");
        check(canaryRunId!=null && canaryRunId.matches("[a-f0-9]{32}") && !canaryRunId.equals(runId),"Invalid preserved CE trial nonce");
        check(CeStorage.enabled(root) && new File(root,CeMigration.LAYOUT32).isFile()
            && new File(root,".pocket-ce-authless").isFile(),"Expected the preserved password-free encrypted profile");
        result.put("probeKind","credential-encryption-authless-resume").put("preservedRunId",canaryRunId);
        long bound=SystemClock.elapsedRealtime()+30000;
        while(!ui(()->runtime!=null)){check(SystemClock.elapsedRealtime()<bound,"Manager service did not bind");Thread.sleep(100);}
        check(!ui(()->runtime.running() || runtime.starting() || runtime.stopping()),"Preserved trial unexpectedly auto-started");
        patchInitTrial();stage("booting-preserved-authless");ui(()->{runtime.startGuest();return null;});boot();
        if(initTrial!=null)initTrial.put("executedSha256",hash(new File(root,"system/bin/pocket-init-api32")));
        checkCrashTransport();awaitCanaries();
        // Replace only this test observer, never the installed runtime or data.
        Files.copy(new File(getApplicationInfo().sourceDir).toPath(),new File(root,"tmp/manager-ce-observer.apk").toPath(),StandardCopyOption.REPLACE_EXISTING);
        stage("verifying-preserved-ce-provider");JSONObject current=app("verify");
        JSONObject lock=keyguard();check(!lock.getBoolean("secure"),"Preserved profile unexpectedly has a lock-screen credential");
        result.put("app",current).put("authlessBootUnlocked",true).put("keyguard",lock);
        canaries(true);result.put("readableCanariesVerified",5);
        if(ExternalVolume32.selected(root)) {
            JSONObject visible=command(Arrays.asList("/system/bin/toybox","cat","/storage/emulated/0/.pocket-ce32-canary"),null,20);
            check(visible.getInt("exit")==0 && visible.getString("output").equals("ce32-canary-"+canaryRunId),"Password-free public emulated canary differs");
            result.put("unlockedExternalCanaryVerified",true);
        }
        capture(0,false);result.put("validation","PASS").put("fullCredentialLifecycleVerified",false)
            .put("scope","Preserved password-free encrypted profile: actual Android user unlock, five-root canaries and ordinary app/keystore persistence after the failed provider-readiness check; no password changes or data reset"+(initTrial==null?"":"; reversible native-init replacement restored after shutdown"));
    }
    private void verifyPreservedKeyguard()throws Exception {
        canaryRunId=getIntent().getStringExtra("ce_preserved_nonce");
        check(canaryRunId!=null && canaryRunId.matches("[a-f0-9]{32}") && !canaryRunId.equals(runId),"Invalid preserved keyguard nonce");
        check(CeStorage.enabled(root) && new File(root,CeMigration.LAYOUT32).isFile()
            && !new File(root,".pocket-ce-authless").exists(),"Expected the preserved password-protected encrypted profile");
        boolean outer=getIntent().getBooleanExtra("ce_keyguard_outer_resume",false);
        boolean restore=getIntent().getBooleanExtra("ce_restore_authless",false);
        check(!restore || !outer,"Password-free restoration requires its own deliberate recovery run");
        result.put("probeKind","credential-encryption-keyguard-resume").put("preservedRunId",canaryRunId)
            .put("outerRestart",outer).put("lockCredentialChangesRequested",restore).put("restoreOriginalPasswordFreeStateRequested",restore)
            .put("fullCredentialLifecycleVerified",false);
        File checkpoint=new File(getFilesDir(),"ce32-keyguard-recovery-"+runId+".json");JSONObject saved=null;
        if(outer) {
            check(checkpoint.isFile() && checkpoint.length()<65536,"Missing bounded keyguard recovery checkpoint");
            saved=new JSONObject(new String(Files.readAllBytes(checkpoint.toPath()),"UTF-8"));
            check(saved.getString("runId").equals(runId) && saved.getString("canaryRunId").equals(canaryRunId)
                && saved.getInt("hostPid")!=android.os.Process.myPid(),"Wrong keyguard recovery restart owner");
            result.put("previousHostPid",saved.getInt("hostPid"));
        }else check(!checkpoint.exists(),"Keyguard recovery checkpoint already exists");
        stage(outer?"booting-keyguard-after-outer-restart":"booting-preserved-keyguard");boot();
        check(ExternalVolume32.selected(root),"Keyguard/volume trial requires the selected mounted backend");
        File statistics=new File(root,".pocket-image-install-stats.json");
        check(statistics.isFile() && statistics.length()<8192,"Missing bounded encrypted-profile image-upgrade statistics");
        result.put("imageInstallStats",new JSONObject(new String(Files.readAllBytes(statistics.toPath()),"UTF-8")));
        checkCrashTransport();
        Files.copy(new File(getApplicationInfo().sourceDir).toPath(),new File(root,"tmp/manager-ce-observer.apk").toPath(),StandardCopyOption.REPLACE_EXISTING);
        canaries(false);result.put("lockedCanariesDenied",5);
        JSONObject locked=command(Arrays.asList("/system/bin/toybox","cat","/storage/emulated/0/.pocket-ce32-canary"),null,20);
        check(locked.getInt("exit")!=0,"Locked public emulated path exposed the encrypted media canary");
        result.put("lockedExternalCanaryDenied",true);
        result.put("keyguardUi",unlockWithKeyguard(passwords[2]));
        // The decrypted filesystem precedes Android's asynchronous user and
        // volume-unlock notifications. Observe those real states before the
        // public-media read, just as before acquiring the ordinary CE provider.
        stage("verifying-preserved-keyguard-app");JSONObject current=app("verify");result.put("app",current);
        check(current.getJSONObject("externalStorage").getBoolean("primaryEmulatedMounted")
            && current.getJSONObject("externalStorage").getBoolean("externalPrimaryQuery"),"Unlocked emulated volume is not mounted/queryable");
        JSONObject visible=command(Arrays.asList("/system/bin/toybox","cat","/storage/emulated/0/.pocket-ce32-canary"),null,20);
        check(visible.getInt("exit")==0 && visible.getString("output").equals("ce32-canary-"+canaryRunId),"Unlocked public emulated canary differs");
        result.put("unlockedExternalCanaryVerified",true);
        if(saved!=null) {
            JSONObject original=saved.getJSONObject("app");
            for(String field:new String[]{"token","cipherDigest","uid","canonicalDataPath"})
                check(current.get(field).equals(original.get(field)),"Encrypted app/keystore changed after outer restart: "+field);
            check(current.getInt("pid")!=original.getInt("pid"),"Encrypted app process did not restart");
            result.put("ordinaryAppPersisted",true);
        }
        if(restore) {
            stage("restoring-original-password-free-state");
            result.put("credentialRemoval",credential("set",passwords[2]+"\n\n",false));
            check(new File(root,".pocket-ce-authless").isFile() && !keyguard().getBoolean("secure"),"Original password-free state was not restored");
            canaries(true);result.put("originalPasswordFreeStateRestored",true).put("readableCanariesVerified",5);
        }
        capture(outer?1:0,false);
        if(!outer && !restore) {
            JSONObject value=new JSONObject().put("runId",runId).put("canaryRunId",canaryRunId)
                .put("hostPid",android.os.Process.myPid()).put("app",current);
            Files.write(checkpoint.toPath(),value.toString().getBytes("UTF-8"),StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE);
        }
        result.put("validation","PASS").put("scope",restore?
            "Preserved five-root encrypted profile: physical password unlock and unchanged app/key/mounted media, then deliberate authenticated removal of this test credential to restore the original password-free state; no data reset":
            "Preserved five-root encrypted profile: visible focused password entry, actual host-input unlock, locked/public-media denial and unlocked read, ordinary app/software Keystore and mounted/queryable MediaProvider; no credential changes or data reset");
    }
    private void publishResult(){try{stage("finished");finished=redact(result.toString());}catch(Exception ignored){}}
    @Override protected void probe() {
        int index=0;
        try {
            result.put("runId",runId).put("sdk",32).put("hostPid",android.os.Process.myPid()).put("probeKind","credential-encryption")
                .put("outerRestart",getIntent().getBooleanExtra("ce_outer_resume",false));
            if(getIntent().getBooleanExtra("ce_authless_verify",false)){verifyPreservedAuthless();publishResult();return;}
            stage("loading-test-credentials");loadTestPasswords();
            if(getIntent().getBooleanExtra("ce_keyguard_resume",false)){verifyPreservedKeyguard();publishResult();return;}
            if(getIntent().getBooleanExtra("ce_outer_resume",false)){outerRestart();publishResult();return;}
            boolean preserved=getIntent().getBooleanExtra("ce_preserved_lifecycle",false);
            File checkpoint=new File(getFilesDir(),"ce32-restart-state.json");JSONObject completed=null;
            if(checkpoint.exists()) {
                String predecessor=getIntent().getStringExtra("ce_completed_predecessor");
                check(preserved && predecessor!=null && predecessor.matches("[a-f0-9]{32}")
                    && checkpoint.length()<65536 && !Files.isSymbolicLink(checkpoint.toPath()),"Existing encryption trial requires deliberate resume");
                completed=new JSONObject(new String(Files.readAllBytes(checkpoint.toPath()),"UTF-8"));
                check(completed.getString("runId").equals(predecessor) && completed.getInt("verifiedBoots")==5
                    && completed.getBoolean("preservedLifecycle") && completed.getString("canaryRunId").equals(getIntent().getStringExtra("ce_preserved_nonce")),
                    "Restart checkpoint does not match the completed predecessor");
                result.put("completedCheckpointPredecessor",predecessor);
            }
            if(preserved) {
                canaryRunId=getIntent().getStringExtra("ce_preserved_nonce");
                check(canaryRunId!=null && canaryRunId.matches("[a-f0-9]{32}") && !canaryRunId.equals(runId),"Invalid preserved lifecycle nonce");
                check(CeStorage.enabled(root) && new File(root,CeMigration.LAYOUT32).isFile()
                    && new File(root,".pocket-ce-authless").isFile(),"Expected the preserved password-free encrypted profile");
                result.put("preservedLifecycle",true).put("preservedRunId",canaryRunId);
            }
            stage(preserved?"booting-preserved-encrypted":"booting-plain");boot();checkCrashTransport();
            Files.copy(new File(getApplicationInfo().sourceDir).toPath(),new File(root,"tmp/manager-ce-observer.apk").toPath(),StandardCopyOption.REPLACE_EXISTING);
            wakeKeyguard("the initial credential lifecycle frame");frame("ce32-plain-"+runId+".png");
            JSONObject original;
            if(preserved) {
                stage("verifying-preserved-lifecycle-app");awaitCanaries();
                JSONObject initial=keyguard();check(!initial.getBoolean("secure"),"Preserved profile still requires a credential");
                result.put("initialAuthlessKeyguard",initial).put("initialAuthlessReadiness",appReadiness(false));
                original=app("verify");
                if(completed!=null) {
                    JSONObject expected=completed.getJSONObject("app");
                    for(String field:new String[]{"token","cipherDigest","uid","canonicalDataPath"})
                        check(original.get(field).equals(expected.get(field)),"Completed checkpoint app/key changed: "+field);
                    File archive=new File(getFilesDir(),"ce32-restart-state-"+completed.getString("runId")+".json");
                    check(!archive.exists(),"Completed restart checkpoint archive already exists");
                    Files.move(checkpoint.toPath(),archive.toPath(),StandardCopyOption.ATOMIC_MOVE);
                    result.put("completedCheckpointArchive",archive.getName()).put("completedCheckpointSha256",hash(archive));
                }
                File statistics=new File(root,".pocket-image-install-stats.json");
                check(statistics.isFile() && statistics.length()<8192,"Missing bounded image-upgrade statistics");
                result.put("imageInstallStats",new JSONObject(new String(Files.readAllBytes(statistics.toPath()),"UTF-8")));
            }
            else {
                stage("installing-ce-app");install();original=app("create");
                for(String base:CeNamespace.API32_ROOTS) {
                    JSONObject write=command(Arrays.asList("/system/bin/sh","-c","umask 077; exec /system/bin/toybox cat > "+base+"/.pocket-ce32-canary"),"ce32-canary-"+runId,20);
                    check(write.getInt("exit")==0,"Cannot create CE data canary");
                }
            }
            result.put("originalApp",original);
            checkInstalldFrontend();
            canaries(true);result.put("originalReadableCanariesVerified",5).put("originalPublicMediaCanaryVerified",publicMediaCanary());
            stage("setting-password");result.put("initialCredential",credential("set","\n"+passwords[0]+"\n",false));
            if(preserved) {check(!new File(root,".pocket-ce-authless").exists(),"Default-unlock slot survived setting a password");capture(index++,false);}
            else {
                stage("preparing-encryption");JSONObject prepared=credential("prepare",passwords[0]+"\n",false);check(prepared.optBoolean("prepared") && prepared.optInt("diskSecretBytes")>=32,"SP disk secret was not prepared");result.put("prepared",prepared);
                capture(index++,true);
            }
            JSONArray boots=new JSONArray();result.put("encryptedBoots",boots);
            for(int phase=0;phase<4;phase++) {
                result.put("encryptedPhase",phase);stage("booting-encrypted");ui(()->{runtime.startGuest();return null;});boot();checkCrashTransport();
                if(phase!=2)wakeKeyguard("the credential lifecycle frame");
                frame("ce32-phase-"+runId+"-"+phase+".png");
                JSONObject bootState=new JSONObject().put("phase",phase);boots.put(bootState);
                if(phase!=2) {
                    canaries(false);bootState.put("lockedCanariesDenied",5);
                    bootState.put("lockedAppReadiness",appReadiness(true));
                    String correct=passwords[phase==0?0:phase==1?1:2];
                    String wrong=phase==0?correct+"x":passwords[phase==1?0:1];
                    credential("check",wrong+"\n",true);canaries(false);bootState.put("wrongPasswordRejected",true);
                    if(phase==3)bootState.put("keyguardUi",unlockWithKeyguard(correct));
                    else {credential("check",correct+"\n",false);awaitCanaries();}
                    bootState.put("credentialUnlock",true);
                }else {awaitCanaries();bootState.put("authlessBootUnlocked",true);}
                bootState.put("unlockedAppReadiness",appReadiness(false));
                stage("verifying-encrypted-app");JSONObject current=app("verify");
                check(current.getString("token").equals(original.getString("token")) && current.getString("cipherDigest").equals(original.getString("cipherDigest"))
                    && current.getInt("uid")==original.getInt("uid") && current.getInt("pid")!=original.getInt("pid"),"Ordinary app private data or keystore changed across encrypted boot");
                bootState.put("app",current);
                canaries(true);bootState.put("readableCanariesVerified",5).put("unlockedExternalCanaryVerified",publicMediaCanary());
                if(phase==0) {stage("changing-password");credential("set",passwords[0]+"\n"+passwords[1]+"\n",false);bootState.put("passwordChanged",true);}
                if(phase==1) {stage("removing-password");credential("set",passwords[1]+"\n\n",false);bootState.put("passwordRemoved",true);}
                if(phase==2) {
                    stage("restoring-password");credential("set","\n"+passwords[2]+"\n",false);
                    check(!new File(root,".pocket-ce-authless").exists(),"Default-unlock slot survived restoring a password");bootState.put("passwordRestored",true);
                }
                Thread.sleep(5000);capture(index++,false);
            }
            JSONObject saved=new JSONObject().put("runId",runId).put("hostPid",android.os.Process.myPid()).put("verifiedBoots",5).put("app",original)
                .put("preservedLifecycle",preserved).put("canaryRunId",canaryRunId==null?runId:canaryRunId);
            Path temporary=new File(getFilesDir(),"ce32-restart-state.json.new").toPath();
            Files.write(temporary,saved.toString().getBytes("UTF-8"),StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE);
            Files.move(temporary,new File(getFilesDir(),"ce32-restart-state.json").toPath(),StandardCopyOption.ATOMIC_MOVE);
            result.put("validation","PASS").put("scope",preserved?
                "Preserved encrypted profile: password set/change/removal/restoration, wrong-password denial, actual keyguard input and unchanged app/keystore across five guest boots; plaintext migration was not repeated":
                "Actual12L LockSettings/vold SP-secret migration, five encrypted roots, wrong-password denial, password change/removal/restoration, real keyguard input and ordinary app/keystore persistence across five guest boots; not an outer Manager process restart");
        }catch(Throwable error) {
            try{
                StringWriter stack=new StringWriter();error.printStackTrace(new PrintWriter(stack));
                result.put("validation","FAIL").put("error",redact(error.toString())).put("errorStack",redact(stack.toString()));
                if(ui(()->runtime!=null && (runtime.running() || runtime.starting() || runtime.stopping())))stop();
                restoreInitTrial();
                JSONObject failed=session(true);failed.put("nativeLog",redact(failed.optString("nativeLog")));
                result.put("failedSession",failed).put("console",redact(console()));
            }catch(Exception cleanup){try{result.put("cleanupError",redact(cleanup.toString()));}catch(Exception ignored){}}
        }
        publishResult();
    }
}
