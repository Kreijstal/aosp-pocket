// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.graphics.Bitmap;
import android.net.Uri;
import android.os.*;
import android.util.Base64;
import android.view.*;
import android.widget.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.regex.*;
import org.json.JSONObject;
import org.json.JSONArray;

/** Exercises the actual Manager service and display, not the component fixture. */
public class ManagerSessionProbe32 extends RuntimeActivity {
    protected File root;
    protected String runId;
    protected volatile String progress="{}",finished;
    protected final JSONObject result=new JSONObject();
    protected static void check(boolean value,String message)throws IOException{if(!value)throw new IOException(message);}
    protected <T>T ui(Callable<T> task)throws Exception {FutureTask<T> call=new FutureTask<>(task);runOnUiThread(call);return call.get(30,TimeUnit.SECONDS);}
    protected void stage(String value)throws Exception {result.put("stage",value);progress=result.toString();}
    protected void beforeSessionStart()throws Exception {}
    @Override public void onCreate(Bundle saved) {
        getIntent().putExtra("guest_sdk",32);autoStart=true;super.onCreate(saved);
        runId=getIntent().getStringExtra("probe_run_id");
        if(runId==null || !runId.matches("[a-f0-9]{32}")){finish();return;}
        setShowWhenLocked(true);setTurnScreenOn(true);
        LinearLayout page=new LinearLayout(this);page.setOrientation(LinearLayout.VERTICAL);
        status=new TextView(this);status.setLines(2);page.addView(status);display=new GuestDisplay(this,false,true);
        page.addView(display,new LinearLayout.LayoutParams(-1,0,1));setContentView(page);
        root=DirectImage.root(this,32);
        try{beforeSessionStart();}catch(Exception error) {
            autoStart=false;
            try{result.put("runId",runId).put("validation","FAIL").put("stage","finished").put("error","Pre-start validation: "+error);finished=result.toString();}
            catch(Exception ignored){}
            return;
        }
        new Thread(this::probe,"PocketManager32Validation").start();
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event){if(runtime!=null && runtime.fullKey(event))return true;return super.dispatchKeyEvent(event);}
    @Override public void dump(String prefix,FileDescriptor fd,PrintWriter writer,String[] args) {
        String value=finished==null?progress:finished;
        writer.println("POCKET_SESSION32="+Base64.encodeToString(value.getBytes(java.nio.charset.StandardCharsets.UTF_8),Base64.NO_WRAP));
    }
    private JSONArray observeInstalldCompiler(AtomicBoolean running,int corePid)throws Exception {
        Map<Integer,JSONObject> seen=new LinkedHashMap<>();
        File directory=new File("/proc");
        while(running.get()) {
            File[] records=directory.listFiles();check(records!=null,"Live process directory is missing");
            for(File record:records)try {
                if(!running.get())break;
                if(!record.getName().matches("[0-9]+"))continue;
                int pid=Integer.parseInt(record.getName());
                if(seen.containsKey(pid))continue;
                String stat=new String(Files.readAllBytes(new File(record,"stat").toPath()),"US-ASCII");
                String[] fields=stat.substring(stat.lastIndexOf(')')+1).trim().split("\\s+");
                if(!stat.startsWith(pid+" (") || fields.length<20)continue;
                int parent=Integer.parseInt(fields[1]);if(parent!=corePid)continue;
                String executable=android.system.Os.readlink("/proc/"+pid+"/exe");
                if(!executable.equals(new File(root,"apex/com.android.art/bin/dex2oat64").getPath())
                    && !executable.equals(new File(root,"apex/com.android.art/bin/dex2oatd64").getPath()))continue;
                if(!HostDns.trustedPeer(root,pid,12347))continue;
                int nice=android.os.Process.getThreadPriority(pid);
                JSONObject value=new JSONObject().put("pid",pid).put("parentPid",parent).put("logicalUid",12347)
                    .put("executable",executable).put("realKernelNice",nice).put("observedUptimeMillis",SystemClock.uptimeMillis());
                try{value.put("timerSlackNanos",Long.parseLong(new String(Files.readAllBytes(new File("/proc/"+pid+"/timerslack_ns").toPath()),"US-ASCII").trim()));}
                catch(IOException unavailable){value.put("timerSlackReadError",unavailable.getClass().getSimpleName());}
                try{value.put("kernelCgroup",new String(Files.readAllBytes(new File("/proc/"+pid+"/cgroup").toPath()),"US-ASCII"));}
                catch(IOException unavailable){value.put("kernelCgroupReadError",unavailable.getClass().getSimpleName());}
                if(HostDns.trustedPeer(root,pid,12347))seen.put(pid,value.put("livePeerVerified",true));
            }catch(IOException | android.system.ErrnoException | NumberFormatException raced){}
            try{Thread.sleep(2);}catch(InterruptedException stopped){if(running.get())throw stopped;break;}
        }
        return new JSONArray(seen.values());
    }
    protected void checkInstalldFrontend()throws Exception {
        InputStream asset;
        try{asset=getAssets().open("api32-installd-client");}
        catch(FileNotFoundException absent){return;}
        File client=new File(root,"tmp/installd-core-client"),smoke=new File(root,"tmp/installd-core-smoke.apk");
        check(!Files.exists(client.toPath(),LinkOption.NOFOLLOW_LINKS)
            && !Files.exists(smoke.toPath(),LinkOption.NOFOLLOW_LINKS),"Inspect previous installd trial files");
        stage("checking-shared-installd");
        JSONObject receipt=new JSONObject();result.put("installdFrontend",receipt);
        try {
            try(InputStream in=asset;OutputStream out=Files.newOutputStream(client.toPath(),StandardOpenOption.CREATE_NEW)){
                GuestImage.copy(in,out,4*1024*1024);
            }
            try(InputStream in=getAssets().open("api32-installd-smoke.apk");
                OutputStream out=Files.newOutputStream(smoke.toPath(),StandardOpenOption.CREATE_NEW)){
                GuestImage.copy(in,out,4*1024*1024);
            }
            java.lang.reflect.Method patch=DirectImage.class.getDeclaredMethod("patchInterpreter",File.class,String.class);
            patch.setAccessible(true);patch.invoke(null,client,new File(root,"system/bin/pocket-loader").getPath());
            android.system.Os.chmod(client.getPath(),0755);
            Matcher core=Pattern.compile("(?m)^POCKET_CORE_START pid=(\\d+) uid=0 components=10 test_echo=0$").matcher(console());
            check(core.find(),"Selected ten-component core did not start");String pid=core.group(1);
            check(!core.find(),"Multiple native core instances");
            Matcher frontend=Pattern.compile("(?m)^POCKET_INSTALLD_FRONTEND_READY pid=(\\d+) uid=0 gid=0 components=core$").matcher(console());
            check(frontend.find() && frontend.group(1).equals(pid) && !frontend.find(),"Installd is not in the root core");
            check(android.system.Os.readlink("/proc/"+pid+"/exe").equals(new File(root,"system/bin/pocket-core-api32").getPath()),
                "Core receipt does not match the live executable");
            receipt.put("corePid",Integer.parseInt(pid)).put("uid",0).put("gid",0).put("sameLiveCoreProcess",true);
            boolean postBoot=false,ordinaryWorker=false;
            try(InputStream in=getAssets().open("api32-installd-postboot")) {
                ByteArrayOutputStream bytes=new ByteArrayOutputStream();GuestImage.copy(in,bytes,32);
                String version=bytes.toString("US-ASCII");
                check(version.equals("v1\n") || version.equals("v2\n"),"Unknown installd post-boot protocol");
                postBoot=true;ordinaryWorker=version.equals("v2\n");
            }catch(FileNotFoundException absent){}
            receipt.put("postBootCompilerRequested",postBoot);
            receipt.put("ordinaryWorkerRequested",ordinaryWorker);
            String[] operations=ordinaryWorker?new String[]{"check","denied","dexopt","dexopt-postboot","dexopt-postboot-app"}:
                postBoot?new String[]{"check","denied","dexopt","dexopt-postboot"}:
                new String[]{"check","denied","dexopt"};
            for(String operation:operations) {
                String shell=". /system/etc/pocket-runtime-env.sh; export LD_PRELOAD=/system/lib64/libpocket_binder_api32.so; exec /tmp/installd-core-client "+operation
                    +(operation.startsWith("dexopt")?" /tmp/installd-core-smoke.apk":"");
                ProcessBuilder builder=new ProcessBuilder(SyscallBackend.command(root,"/",Arrays.asList("/system/bin/sh","-c",shell))).directory(root);
                builder.environment().clear();builder.environment().putAll(SyscallBackend.environment(root,android.os.Process.myPid(),32));
                File log=new File(root,"logs/installd-core-"+operation+".log");
                boolean observeWorker=operation.equals("dexopt-postboot-app");
                AtomicBoolean observing=new AtomicBoolean(observeWorker);
                FutureTask<JSONArray> observed=new FutureTask<>(()->observeInstalldCompiler(observing,Integer.parseInt(pid)));
                Thread observer=new Thread(observed,"PocketCompilerObserver");
                int parentNiceBefore=android.os.Process.getThreadPriority(Integer.parseInt(pid));
                if(observeWorker)observer.start();
                java.lang.Process process=null;
                try {
                    builder.redirectErrorStream(true).redirectOutput(log);process=builder.start();
                    boolean exited=process.waitFor(120,TimeUnit.SECONDS);
                    check(log.length()<131072,"Installd client output exceeds capture limit");
                    String text=new String(Files.readAllBytes(log.toPath()),"UTF-8");
                    receipt.put(operation,new JSONObject().put("exited",exited)
                        .put("exit",exited?process.exitValue():-1).put("output",text));
                    check(exited && process.exitValue()==0,"Installd "+operation+" failed: "+text);
                    String marker=operation.equals("check")?"API32_INSTALLD_PASS stock_daemon=1 caller_uid=1000 create_size_clear_destroy=1":
                        operation.equals("denied")?"API32_INSTALLD_ROLE_DENIED_PASS uid=39999 operation=invalidateMounts":
                        "API32_INSTALLD_DEXOPT_PASS stock_dex2oat=1 filter=verify odex_elf=1 vdex=1 cleanup=1 boot_time="
                            +(operation.startsWith("dexopt-postboot")?"0":"1");
                    check(text.contains(marker),"Missing installd "+operation+" receipt");
                }finally{
                    if(process!=null && process.isAlive())process.destroyForcibly();
                    if(observeWorker){
                        observing.set(false);observer.interrupt();JSONArray workers=observed.get(5,TimeUnit.SECONDS);observer.join(1000);
                        check(!observer.isAlive(),"Compiler observer did not stop");
                        receipt.put("ordinaryCompilerWorkers",workers);
                        int parentNiceAfter=android.os.Process.getThreadPriority(Integer.parseInt(pid));
                        receipt.put("coreNiceBefore",parentNiceBefore).put("coreNiceAfter",parentNiceAfter);
                        check(workers.length()>0,"No live ordinary compiler worker observed");
                        for(int i=0;i<workers.length();i++)check(workers.getJSONObject(i).getInt("realKernelNice")==10,"Post-boot compiler did not enter background priority");
                        check(parentNiceBefore==parentNiceAfter,"Core priority changed during compiler request");
                    }
                }
            }
            receipt.put("validation","PASS");
        }finally{Files.deleteIfExists(client.toPath());Files.deleteIfExists(smoke.toPath());}
    }
    protected void boot()throws Exception {
        long deadline=SystemClock.elapsedRealtime()+900000;
        while(SystemClock.elapsedRealtime()<deadline) {
            if(ui(()->runtime!=null && runtime.booted())){check(ui(()->runtime.selectedSdk())==32,"Wrong Manager SDK");return;}
            String message=ui(()->runtime==null?"":runtime.status());
            if(message.contains("failed:") || message.contains("cleanup failed:"))throw new IOException(message);
            result.put("managerStatus",message);progress=result.toString();Thread.sleep(500);
        }
        throw new IOException("Manager did not boot 12L");
    }
    protected JSONObject session(boolean logs)throws Exception{return ui(()->runtime.sessionReport(logs));}
    protected String nativeLog()throws Exception {return session(true).optString("nativeLog");}
    private String await(String marker)throws Exception {
        long until=SystemClock.elapsedRealtime()+90000;
        while(SystemClock.elapsedRealtime()<until){
            String text=nativeLog();if(text.contains(marker))return text;
            File operation=new File(root,"logs/manager-operation.log");
            if(operation.isFile() && operation.length()<2*1024*1024) {
                String output=new String(Files.readAllBytes(operation.toPath()),"UTF-8");
                if(output.contains("POCKET_MANAGER32_ERROR"))throw new IOException(output.length()>2000?output.substring(0,2000):output);
            }
            Thread.sleep(250);
        }
        throw new IOException("Guest did not report "+marker);
    }
    protected int frame(String name)throws Exception {
        long until=SystemClock.elapsedRealtime()+60000;
        while(SystemClock.elapsedRealtime()<until) {
            Bitmap bitmap=ui(()->runtime.snapshot());
            if(bitmap!=null) {
                Set<Integer> colors=new HashSet<>();
                for(int y=0;y<bitmap.getHeight();y+=11)for(int x=0;x<bitmap.getWidth();x+=11)colors.add(bitmap.getPixel(x,y));
                if(colors.size()>8){try(OutputStream out=new FileOutputStream(new File(getFilesDir(),name))){bitmap.compress(Bitmap.CompressFormat.PNG,100,out);}bitmap.recycle();return colors.size();}
                bitmap.recycle();
            }
            Thread.sleep(250);
        }
        throw new IOException("Manager did not present a nontrivial compositor frame");
    }
    protected void stop()throws Exception {
        ui(()->{if(runtime!=null)runtime.stopGuest();return null;});long until=SystemClock.elapsedRealtime()+30000;
        while(ui(()->runtime!=null && (runtime.running() || runtime.starting() || runtime.stopping()))) {
            check(SystemClock.elapsedRealtime()<until,"Manager did not stop its guest");Thread.sleep(100);
        }
        check(DirectImage.leftovers(root).isEmpty(),"Guest processes remain after stop");
    }
    protected String console()throws IOException {
        File file=new File(root,"logs/manager-init.log");
        if(!file.isFile())return "";
        check(file.length()<2*1024*1024,"Manager console exceeds capture limit");
        return new String(Files.readAllBytes(file.toPath()),"UTF-8");
    }
    protected void checkCrashTransport()throws Exception {
        int target=-1;
        for(int pid:DirectImage.leftovers(root)) {
            String executable;
            try{executable=android.system.Os.readlink("/proc/"+pid+"/exe");}
            catch(android.system.ErrnoException exited) {
                if(exited.errno==android.system.OsConstants.ENOENT || exited.errno==android.system.OsConstants.ESRCH)continue;
                throw exited;
            }
            if(executable.endsWith("/system/bin/tombstoned")){check(target==-1,"Multiple crash-reporting daemons");target=pid;}
        }
        check(target>1,"No owned crash-reporting daemon");
        File client=new File(root,"tmp/tombstoned-manager-client");
        try(InputStream in=getAssets().open("api32-tombstoned-client");OutputStream out=new FileOutputStream(client)){GuestImage.copy(in,out,1024*1024);}
        java.lang.reflect.Method patch=DirectImage.class.getDeclaredMethod("patchInterpreter",File.class,String.class);patch.setAccessible(true);
        patch.invoke(null,client,new File(root,"system/bin/pocket-loader").getPath());android.system.Os.chmod(client.getPath(),0755);
        try {
            for(String mode:new String[]{"denied","outside"}) {
                File log=new File(root,"logs/tombstoned-client-"+mode+".log");
                ProcessBuilder builder=new ProcessBuilder(SyscallBackend.command(root,"/",Arrays.asList("/tmp/tombstoned-manager-client",mode,String.valueOf(target)))).directory(root);
                builder.environment().clear();builder.environment().putAll(SyscallBackend.environment(root,android.os.Process.myPid(),32));
                builder.redirectErrorStream(true).redirectOutput(log);java.lang.Process process=builder.start();
                String text;
                try {
                    boolean exited=process.waitFor(20,TimeUnit.SECONDS);
                    check(log.length()<65536,"Crash rejection client log exceeds capture limit");
                    text=new String(Files.readAllBytes(log.toPath()),"UTF-8");
                    result.put("tombstonedClient_"+mode,new JSONObject().put("exited",exited)
                        .put("exitCode",exited?process.exitValue():-1).put("serverPid",target).put("log",text));
                    check(exited && process.exitValue()==0,"Crash transport did not reject "+mode+" client: exit="
                        +(exited?process.exitValue():"running")+" log="+text.substring(Math.max(0,text.length()-2000)));
                }
                finally{if(process.isAlive())process.destroyForcibly();}
                check(text.contains(mode.equals("denied")?"API32_TOMBSTONED_ROLE_DENIED_PASS":"API32_TOMBSTONED_OUTSIDE_PROFILE_DENIED_PASS"),"Missing crash-transport rejection proof");
                result.put("tombstoned_"+mode,text.trim());
            }
        }finally{Files.deleteIfExists(client.toPath());}
    }
    protected void probe() {
        try {
            result.put("runId",runId).put("sdk",32);stage("booting");boot();checkCrashTransport();
            result.put("homeColors",frame("manager32-home.png"));stage("installing");
            File apk=new File(getFilesDir(),"manager32-test.apk");
            try(InputStream in=getAssets().open("api32-manager-smoke.apk");OutputStream out=new FileOutputStream(apk)){GuestImage.copy(in,out,1024*1024);}
            Files.deleteIfExists(new File(root,"logs/manager-operation.log").toPath());
            ui(()->{runtime.installApps(Collections.singletonMap("base.apk",Uri.fromFile(apk)));return null;});
            String text=await("POCKET_GUEST_BUTTON center=");
            Matcher button=Pattern.compile("POCKET_GUEST_BUTTON center=([0-9]+),([0-9]+)").matcher(text);check(button.find(),"Guest button coordinates missing");
            int x=Integer.parseInt(button.group(1)),y=Integer.parseInt(button.group(2));
            JSONObject target=ui(()->{
                View surface=display.getChildAt(0);int[] location=new int[2];surface.getLocationOnScreen(location);
                check(surface.isShown() && surface.getWidth()>0 && surface.getHeight()>0,"Host input surface is not visible");
                return new JSONObject().put("x",location[0]+x*surface.getWidth()/runtime.width)
                    .put("y",location[1]+y*surface.getHeight()/runtime.height);
            });
            result.put("hostTapTarget",target);stage("awaiting-host-tap");
            await("PocketGuestTest:tap=1");result.put("appColors",frame("manager32-app.png")).put("installAndTouch",true);
            ui(()->{runtime.key(KeyEvent.KEYCODE_HOME);return null;});Thread.sleep(1000);
            check(ui(()->runtime.hasLastApp()),"Installed-package preference missing");
            ui(()->{runtime.openLastApp();return null;});await("POCKET_GUEST_BUTTON center=");
            stage("observing");Thread.sleep(40000);
            File performance=new File(root,"logs/display-performance.json");
            check(performance.isFile(),"Manager did not record a display presentation");
            JSONObject frames=new JSONObject(new String(Files.readAllBytes(performance.toPath()),"UTF-8"));
            check(frames.getLong("frames_presented")>0,"Manager never presented frames to its host Surface");
            result.put("display",frames);
            JSONObject active=session(true);String activeLog=active.optString("nativeLog");active.remove("nativeLog");
            result.put("activeSession",active).put("console",console());
            JSONObject input=active.getJSONObject("input");
            check(input.getLong("touchEventsAccepted")>=2 && input.getLong("keyEventsAccepted")>=2 && input.getLong("inputEventsRejected")==0,"Input did not reach API32 InputManager");
            check(activeLog.length()<2*1024*1024,"Guest native-log capture limit reached");
            result.put("functionalPassed",true);
        }catch(Throwable error){try{result.put("functionalPassed",false).put("error",error.toString());}catch(Exception ignored){}}
        finally {
            try{stage("stopping");stop();JSONObject stopped=session(false);result.put("stoppedSession",stopped);}
            catch(Exception error){try{result.put("functionalPassed",false).put("cleanupError",error.toString());}catch(Exception ignored){}}
            try {
                result.put("console",console());JSONObject stopped=session(true);
                String logs=stopped.optString("nativeLog");result.put("nativeLog",logs);
                boolean startupClean=!logs.contains("ANR in ") && !logs.contains("FATAL EXCEPTION:")
                    && !logs.contains("Fatal signal ") && logs.length()<2*1024*1024;
                result.put("startupClean",startupClean);
                boolean cleanup=stopped.has("ownerExitCode") && stopped.getInt("ownerExitCode")==0
                    && !stopped.optBoolean("ownerAlive",true) && !stopped.has("cleanupFailure");
                result.put("supervisedStop",cleanup);
                result.put("validation",result.optBoolean("functionalPassed") && startupClean && cleanup?"PASS":"FAIL");
                result.put("stage","finished");finished=result.toString();
                Files.write(new File(getFilesDir(),"manager32-session-result.json").toPath(),finished.getBytes("UTF-8"));
            }catch(Exception error){try{result.put("validation","FAIL").put("reportError",error.toString());finished=result.toString();}catch(Exception ignored){}}
        }
    }
}
