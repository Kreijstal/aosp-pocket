// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.graphics.Bitmap;
import android.net.Uri;
import android.os.SystemClock;
import android.view.KeyEvent;
import java.io.*;
import java.net.*;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.*;
import org.json.*;

/** Unmodified external split APKs through the real Manager install/display path. */
public final class ManagerAppProbe32 extends ManagerSessionProbe32 {
    private JSONObject spec;
    private File storageRequest;
    private File storageOwnerRequest;
    private File storagePhaseRequest;
    private JSONObject ownerSeed;
    private boolean volumeNeedsRemount;
    private boolean unmountAttempted;
    private JSONObject crashCheckpoint,crashPreStart;
    private boolean crashResume;
    private boolean readTombstonesOnly;
    private volatile boolean crashArmed;
    private File crashFile(String suffix){return new File(getFilesDir(),"manager-crash32-"+runId+suffix+".json");}
    private static String processStart(int pid)throws IOException {
        String text=new String(Files.readAllBytes(new File("/proc/"+pid+"/stat").toPath()),"US-ASCII");
        String[] fields=text.substring(text.lastIndexOf(')')+1).trim().split("\\s+");
        check(text.startsWith(pid+" (") && fields.length>=20,"Invalid kernel process record");return fields[19];
    }
    private static boolean sameProcess(JSONObject saved) {
        try{return processStart(saved.getInt("pid")).equals(saved.getString("startTicks"));}
        catch(Exception gone){return false;}
    }
    private JSONArray crashEndpoints()throws Exception {
        JSONArray records=new JSONArray();
        for(String name:new String[]{"dev/socket/pocket_volume32","dev/socket/pocket_ext","dev/socket/pocket_efs","dev/socket/pocket_xfd",".pocket-external-ready",".pocket-external-device"}) {
            File file=new File(root,name);JSONObject value=new JSONObject().put("path",name);
            if(Files.exists(file.toPath(),LinkOption.NOFOLLOW_LINKS)) {
                android.system.StructStat stat=android.system.Os.lstat(file.getPath());
                value.put("present",true).put("mode",stat.st_mode&android.system.OsConstants.S_IFMT).put("inode",stat.st_ino).put("device",stat.st_dev);
            }else value.put("present",false);
            records.put(value);
        }
        return records;
    }
    private void writeCrashRecord(File file,JSONObject value)throws Exception {
        java.io.FileDescriptor fd=android.system.Os.open(file.getPath(),android.system.OsConstants.O_WRONLY
            |android.system.OsConstants.O_CREAT|android.system.OsConstants.O_EXCL|android.system.OsConstants.O_CLOEXEC,0600);
        try(FileOutputStream stream=new FileOutputStream(fd)){stream.write(value.toString().getBytes("UTF-8"));stream.getFD().sync();}
        fd=android.system.Os.open(getFilesDir().getPath(),android.system.OsConstants.O_RDONLY|android.system.OsConstants.O_CLOEXEC,0);
        try{check((android.system.Os.fstat(fd).st_mode&android.system.OsConstants.S_IFMT)==android.system.OsConstants.S_IFDIR,"Checkpoint parent is not a directory");
            android.system.Os.fsync(fd);}finally{android.system.Os.close(fd);}
    }
    @Override protected void beforeSessionStart()throws Exception {
        // Read only marker existence, never credentials or key material. This
        // distinguishes an actual plaintext migration candidate from a
        // password-free profile that is already encrypted.
        result.put("storageEncryptionEnabledBeforeStart",CeStorage.enabled(root))
            .put("fiveRootEncryptionLayoutBeforeStart",new File(root,CeMigration.LAYOUT32).isFile());
        try(InputStream in=getAssets().open("api32-app-spec.json")) {
            ByteArrayOutputStream bytes=new ByteArrayOutputStream();GuestImage.copy(in,bytes,65536);spec=new JSONObject(bytes.toString("UTF-8"));
        }
        if(getIntent().getBooleanExtra("read_failed_tombstones_only",false)) {
            autoStart=false;readTombstonesOnly=true;
            check(DirectImage.leftovers(root).isEmpty(),"Cannot read stopped-run artifacts while a guest is active");
            return;
        }
        if(!spec.optBoolean("managerCrashRecovery"))return;
        check(spec.optBoolean("requireKeyCanary") && spec.has("storageOwner") && !spec.optBoolean("volumeLifecycle"),"Invalid crash fixture");
        File saved=crashFile("-checkpoint"),signal=crashFile("-signal");
        if(!Files.exists(saved.toPath(),LinkOption.NOFOLLOW_LINKS)) {
            check(!Files.exists(signal.toPath(),LinkOption.NOFOLLOW_LINKS),"Unowned crash signal record");return;
        }
        check(saved.isFile() && saved.length()<4*1024*1024 && signal.isFile() && signal.length()<8192,"Incomplete crash checkpoint; inspect before restarting");
        crashCheckpoint=new JSONObject(new String(Files.readAllBytes(saved.toPath()),"UTF-8"));
        JSONObject request=new JSONObject(new String(Files.readAllBytes(signal.toPath()),"UTF-8"));
        check(crashCheckpoint.getString("runId").equals(runId) && request.getString("runId").equals(runId)
            && request.getInt("signal")==9 && request.getString("checkpointSha256").equals(digest(saved))
            && request.getInt("pid")==crashCheckpoint.getInt("hostPid"),"Crash checkpoint/signal mismatch");
        check(crashCheckpoint.getInt("hostPid")!=android.os.Process.myPid() && !sameProcess(crashCheckpoint.getJSONObject("hostProcess")),"Original Manager process is still alive");
        check(crashCheckpoint.getString("managerSha256").equals(digest(new File(getApplicationInfo().sourceDir)))
            && crashCheckpoint.getJSONObject("body").getJSONObject("appSpec").toString().equals(spec.toString()),"Crash APK/spec changed");
        crashResume=true;JSONArray processes=new JSONArray();
        for(int i=0;i<crashCheckpoint.getJSONArray("processes").length();i++) {
            JSONObject old=crashCheckpoint.getJSONArray("processes").getJSONObject(i);
            processes.put(new JSONObject(old.toString()).put("sameProcessAlive",sameProcess(old)));
        }
        crashPreStart=new JSONObject().put("hostPid",android.os.Process.myPid()).put("endpoints",crashEndpoints())
            .put("oldProcesses",processes).put("signalRequest",request).put("checkpointSha256",digest(saved));
        boolean stale=false;
        for(int i=0;i<crashPreStart.getJSONArray("endpoints").length();i++) {
            JSONObject endpoint=crashPreStart.getJSONArray("endpoints").getJSONObject(i);
            if(endpoint.getString("path").equals("dev/socket/pocket_volume32"))stale=endpoint.optBoolean("present")
                && endpoint.getInt("mode")==android.system.OsConstants.S_IFSOCK;
        }
        check(stale,"No stale volume socket remained after the abrupt Manager death");
    }
    private void awaitManagerCrash()throws Exception {
        stage("holding-crash-descriptor");ownerAction("hold");openOwner();result.put("crashHeldOwner",ownerProof("hold"));
        JSONObject live=session(true);check(live.getBoolean("running") && live.getBoolean("ready") && live.getJSONObject("externalStorage").getBoolean("attached"),"Guest is not live before crash");
        result.put("preCrashSession",live).put("preCrashConsole",console()).put("preCrashNativeLog",nativeLog());
        JSONArray processes=new JSONArray();
        for(int pid:DirectImage.leftovers(root))try {
            processes.put(new JSONObject().put("pid",pid).put("startTicks",processStart(pid)).put("executable",android.system.Os.readlink("/proc/"+pid+"/exe")));
        }catch(IOException | android.system.ErrnoException exited){}
        check(processes.length()>=10,"Missing live guest process inventory");
        int host=android.os.Process.myPid();
        JSONObject saved=new JSONObject().put("runId",runId).put("hostPid",host)
            .put("hostProcess",new JSONObject().put("pid",host).put("startTicks",processStart(host)))
            .put("managerSha256",digest(new File(getApplicationInfo().sourceDir))).put("processes",processes)
            .put("endpoints",crashEndpoints()).put("body",new JSONObject(result.toString()));
        writeCrashRecord(crashFile("-checkpoint"),saved);
        result.put("crashCheckpointSha256",digest(crashFile("-checkpoint"))).put("managerHostPid",host).put("managerCrashResume",false);
        crashArmed=true;stage("awaiting-manager-crash");
        Thread.sleep(180000);throw new IOException("No authenticated Manager crash request arrived");
    }
    @Override public void dump(String prefix,FileDescriptor fd,PrintWriter writer,String[] args) {
        if(args.length==2 && args[0].equals("pocket-crash32") && args[1].equals(runId)) {
            try {
                check(crashArmed && result.optString("stage").equals("awaiting-manager-crash"),"Crash is not armed");
                int pid=android.os.Process.myPid();
                writeCrashRecord(crashFile("-signal"),new JSONObject().put("runId",runId).put("pid",pid).put("signal",9)
                    .put("checkpointSha256",digest(crashFile("-checkpoint"))).put("elapsedMillis",SystemClock.elapsedRealtime()));
                crashArmed=false;writer.println("POCKET_CRASH32_SIGKILL_ARMED pid="+pid);writer.flush();
                new Thread(()->{try{Thread.sleep(100);android.system.Os.kill(pid,android.system.OsConstants.SIGKILL);}
                    catch(Exception error){android.util.Log.e("PocketCrash32","Self SIGKILL failed",error);}},"PocketSelfSigkill").start();
            }catch(Exception error){writer.println("POCKET_CRASH32_REJECTED "+error);}
            return;
        }
        super.dump(prefix,fd,writer,args);
    }
    private void verifyCrashRestart()throws Exception {
        check(crashResume && crashCheckpoint!=null,"Missing crash predecessor");JSONArray gone=new JSONArray();
        for(int i=0;i<crashCheckpoint.getJSONArray("processes").length();i++) {
            JSONObject old=crashCheckpoint.getJSONArray("processes").getJSONObject(i);check(!sameProcess(old),"Old guest process survived recovery: "+old);
            gone.put(old);
        }
        JSONObject live=session(false),old=crashCheckpoint.getJSONObject("body").getJSONObject("preCrashSession");
        check(live.getJSONObject("externalStorage").getBoolean("attached")
            && live.getJSONObject("externalStorage").getLong("epoch")!=old.getJSONObject("externalStorage").getLong("epoch"),"Mounted storage epoch was not renewed");
        result.put("managerCrashPreStart",crashPreStart).put("managerCrashCheckpoint",crashCheckpoint)
            .put("managerCrashOldProcessesGone",gone).put("managerCrashNewSession",live);
    }
    private JSONObject cameraHostRequest(int operation)throws Exception {
        check(operation==dev.kreijstal.aosppocket.bridge.HostWire.CAMERAS
            || operation==dev.kreijstal.aosppocket.bridge.HostWire.CAMERA_STREAM,"Invalid camera permission probe");
        try(android.net.LocalSocket socket=new android.net.LocalSocket()) {
            socket.connect(new android.net.LocalSocketAddress(new File(root,"dev/socket/pocket_host").getPath(),android.net.LocalSocketAddress.Namespace.FILESYSTEM));
            socket.setSoTimeout(5000);
            DataOutputStream out=new DataOutputStream(socket.getOutputStream());DataInputStream in=new DataInputStream(socket.getInputStream());
            out.writeInt(dev.kreijstal.aosppocket.bridge.HostWire.MAGIC);out.writeInt(operation);
            dev.kreijstal.aosppocket.bridge.HostWire.write(out,"{\"format\":\"lines\"}".getBytes("UTF-8"));out.flush();
            int status=in.readInt();byte[] reply=dev.kreijstal.aosppocket.bridge.HostWire.read(in,65536);int cameras=0;
            for(String line:new String(reply,"UTF-8").split("\n"))if(line.startsWith("camera "))cameras++;
            return new JSONObject().put("status",status).put("replyBytes",reply.length).put("cameraCount",cameras);
        }
    }
    private void awaitHostCameraGrant()throws Exception {
        check(spec.optString("proof").equals("media-av-v1"),"Late camera grant requires the AV fixture");
        check(!HostBroker.permitted(this,android.Manifest.permission.CAMERA),"Host camera permission was granted before boot");
        check(ui(()->runtime.booted()),"Guest must be booted before host camera grant");
        result.put("cameraHostPermissionDeniedBeforeGrant",true);
        result.put("cameraPermissionGuestRunIdBefore",session(false).getString("runId"));
        JSONObject inventory=cameraHostRequest(dev.kreijstal.aosppocket.bridge.HostWire.CAMERAS);
        JSONObject denied=cameraHostRequest(dev.kreijstal.aosppocket.bridge.HostWire.CAMERA_STREAM);
        result.put("cameraInventoryBeforeHostGrant",inventory).put("cameraStreamBeforeHostGrant",denied);
        check(inventory.getInt("status")==0 && inventory.getInt("cameraCount")>0,"Public camera inventory unavailable without host grant");
        check(denied.getInt("status")==403,"Camera capture was not denied without host grant");
        stage("awaiting-host-camera-grant");long deadline=SystemClock.elapsedRealtime()+90000;
        while(!HostBroker.permitted(this,android.Manifest.permission.CAMERA)) {
            check(SystemClock.elapsedRealtime()<deadline,"Host camera grant timed out");Thread.sleep(100);
        }
        result.put("cameraHostPermissionGrantedAfterBoot",true);
        result.put("cameraPermissionGuestRunIdAfter",session(false).getString("runId"));
        check(result.getString("cameraPermissionGuestRunIdBefore").equals(result.getString("cameraPermissionGuestRunIdAfter"))
            && ui(()->runtime.booted()),"Guest changed during host camera grant");
    }
    private static String digest(File file)throws Exception {
        MessageDigest hash=MessageDigest.getInstance("SHA-256");byte[] bytes=new byte[65536];int count;
        try(InputStream in=new FileInputStream(file)){while((count=in.read(bytes))!=-1)hash.update(bytes,0,count);}
        StringBuilder value=new StringBuilder();for(byte b:hash.digest())value.append(String.format("%02x",b&255));return value.toString();
    }
    private Map<String,Uri> download()throws Exception {
        return download(spec,"compat32");
    }
    private Map<String,Uri> download(JSONObject spec,String prefix)throws Exception {
        File archive=new File(getFilesDir(),prefix+".apks");
        URL url=new URL(spec.getString("url"));
        check(url.getProtocol().equals("http") && url.getHost().equals("127.0.0.1") && url.getPort()==8794,"Expected isolated test download");
        HttpURLConnection connection=(HttpURLConnection)url.openConnection();connection.setConnectTimeout(10000);connection.setReadTimeout(60000);
        connection.setInstanceFollowRedirects(false);
        try {
            check(connection.getResponseCode()==200,"App download did not succeed");
            try(InputStream in=connection.getInputStream();OutputStream out=new FileOutputStream(archive)){GuestImage.copy(in,out,spec.getLong("archiveBytes"));}
        }finally{connection.disconnect();}
        check(archive.length()==spec.getLong("archiveBytes") && digest(archive).equals(spec.getString("archiveSha256")),"APK set digest mismatch");
        Map<String,Uri> files=new LinkedHashMap<>();JSONArray entries=spec.getJSONArray("files");
        try(ZipFile zip=new ZipFile(archive)) {
            check(zip.size()==entries.length() && entries.length()>0 && entries.length()<=128,"Unexpected APK set entries");
            for(int i=0;i<entries.length();i++) {
                JSONObject entry=entries.getJSONObject(i);String name=entry.getString("name");
                check(name.matches("[a-zA-Z0-9_.-]+\\.apk") && !files.containsKey(name),"Invalid APK filename");
                ZipEntry encoded=zip.getEntry(name);check(encoded!=null && encoded.getSize()==entry.getLong("bytes"),"Wrong APK size");
                File file=new File(getFilesDir(),prefix+"-"+name);
                try(InputStream in=zip.getInputStream(encoded);OutputStream out=new FileOutputStream(file)){GuestImage.copy(in,out,entry.getLong("bytes"));}
                check(file.length()==entry.getLong("bytes") && digest(file).equals(entry.getString("sha256")),"APK digest mismatch: "+name);
                files.put(name,Uri.fromFile(file));
            }
        }
        check(files.containsKey("base.apk"),"Base APK missing");return files;
    }
    private JSONObject inspect()throws Exception {
        return inspect(spec.getString("package"));
    }
    private JSONObject inspect(String name)throws Exception {
        check(name.equals("--activity-policy") || name.matches("[a-zA-Z][a-zA-Z0-9_]*(?:\\.[a-zA-Z0-9_]+)+"),"Invalid observer package");
        File log=new File(root,"logs/app-inspect.log");
        String shell=". /system/etc/pocket-runtime-env.sh; export LD_PRELOAD=/system/lib64/libpocket_binder_api32.so; exec /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /tmp/manager-app-inspector.apk /system/bin dev.kreijstal.aosppocket.guest.AppInspector32 "+name;
        ProcessBuilder builder=new ProcessBuilder(SyscallBackend.command(root,"/",Arrays.asList("/system/bin/sh","-c",shell))).directory(root);
        builder.environment().clear();builder.environment().putAll(SyscallBackend.environment(root,android.os.Process.myPid(),32));
        builder.redirectErrorStream(true).redirectOutput(log);java.lang.Process process=builder.start();
        try {
            check(process.waitFor(60,TimeUnit.SECONDS),"Framework observer timed out");
            check(log.length()<2*1024*1024,"Observer log exceeds capture limit");
            String text=new String(Files.readAllBytes(log.toPath()),"UTF-8");
            check(process.exitValue()==0,"Framework observer failed: "+text.substring(Math.max(0,text.length()-2000)));
            for(String line:text.split("\n"))if(line.startsWith("POCKET_APP32_INSPECT "))return new JSONObject(line.substring(21));
            throw new IOException("Observer returned no result");
        }finally{if(process.isAlive())process.destroyForcibly();}
    }
    private JSONObject cryptoFrontend()throws Exception {
        File client=new File(root,"tmp/gatekeeper-frontend-client");
        try(InputStream in=getAssets().open("api32-gatekeeper-client");OutputStream out=new FileOutputStream(client)) {
            GuestImage.copy(in,out,4*1024*1024);
        }
        java.lang.reflect.Method patch=DirectImage.class.getDeclaredMethod("patchInterpreter",File.class,String.class);patch.setAccessible(true);
        patch.invoke(null,client,new File(root,"system/bin/pocket-loader").getPath());android.system.Os.chmod(client.getPath(),0755);
        String shell=". /system/etc/pocket-runtime-env.sh; export LD_PRELOAD=/system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so; exec /tmp/gatekeeper-frontend-client shared-frontend";
        ProcessBuilder builder=new ProcessBuilder(SyscallBackend.command(root,"/",Arrays.asList("/system/bin/sh","-c",shell))).directory(root);
        builder.environment().clear();builder.environment().putAll(SyscallBackend.environment(root,android.os.Process.myPid(),32));
        File log=new File(root,"logs/crypto-frontend-client.log");builder.redirectErrorStream(true).redirectOutput(log);java.lang.Process process=builder.start();
        try {
            check(process.waitFor(120,TimeUnit.SECONDS),"Credential frontend diagnostic timed out");
            check(log.length()<65536,"Credential frontend diagnostic output exceeds limit");
            String text=new String(Files.readAllBytes(log.toPath()),"UTF-8");
            check(process.exitValue()==0,"Credential frontend diagnostic failed: "+text);
            check(text.contains("API32_GATEKEEPER_ROLE_DENIED_PASS ") && text.contains("API32_GATEKEEPER_PASS ")
                && text.contains("API32_GATEKEEPER_REENROLL_PASS "),"Credential lifecycle receipt missing");
            return new JSONObject().put("exit",process.exitValue()).put("output",text);
        }finally{if(process.isAlive())process.destroyForcibly();Files.deleteIfExists(client.toPath());}
    }
    private boolean awaitAppReadiness(boolean diagnostic)throws Exception {
        File marker=new File(root,"system/etc/pocket-app-readiness32");
        if(!marker.exists()){check(!diagnostic,"App readiness marker is missing");return false;}
        check(marker.length()<=64 && new String(Files.readAllBytes(marker.toPath()),"US-ASCII")
            .equals("boot-broadcasts-and-volume-v1\n"),"Unsupported app readiness marker");
        check(GuestManager32.selected(root),"App readiness requires owner control");
        stage("observing-app-readiness");result.put("startupHomeColors",frame("startup-home.png"));
        Bitmap bitmap=ui(()->runtime.snapshot());check(bitmap!=null,"Missing startup geometry");
        int width=bitmap.getWidth(),height=bitmap.getHeight();bitmap.recycle();
        JSONArray changes=new JSONArray();result.put("startupReadiness",changes);
        long deadline=SystemClock.elapsedRealtime()+180000;String previous=null;int owner=0,queries=0;
        for(;;) {
            JSONObject state=new JSONObject(GuestManager32.execute(root,width,height,java.util.concurrent.ConcurrentHashMap.newKeySet(),
                ()->false,new File(root,"logs/startup-readiness.log"),new String[]{"startup"}));
            queries++;
            check(state.getInt("schema")==1 && state.getInt("sdk")==32 && state.getInt("uid")==1000
                && state.getString("source").equals("LocalServices.UserController.mBootCompleted"),"Invalid startup state source");
            int pid=state.getInt("pid");check(pid>0 && (owner==0 || owner==pid),"Startup observer changed owner");owner=pid;
            boolean ready=state.getBoolean("bootBroadcastComplete") && state.getBoolean("userUnlocked")
                && (!state.getBoolean("externalVolumeRequired") || state.getBoolean("primaryVolumeMounted"));
            check(state.getBoolean("readyForApps")==ready,"Inconsistent startup readiness");
            String key=""+state.getBoolean("bootBroadcastComplete")+state.getBoolean("userUnlocked")+state.getBoolean("primaryVolumeMounted");
            if(!key.equals(previous)){changes.put(state);previous=key;progress=result.toString();}
            if(queries==1 && diagnostic) {
                long before=session(false).getJSONObject("input").getLong("keyEventsAccepted");
                ui(()->{runtime.key(KeyEvent.KEYCODE_HOME);return null;});long until=SystemClock.elapsedRealtime()+10000;
                // GuestInput32 sends WAKEUP down/up followed by HOME down/up.
                // Wait for all four real injectInputEvent replies, not just wake.
                while(session(false).getJSONObject("input").getLong("keyEventsAccepted")<before+4) {
                    check(SystemClock.elapsedRealtime()<until,"Early startup input was not accepted");Thread.sleep(50);
                }
                JSONObject after=new JSONObject(GuestManager32.execute(root,width,height,java.util.concurrent.ConcurrentHashMap.newKeySet(),
                    ()->false,new File(root,"logs/startup-after-input.log"),new String[]{"startup"}));
                result.put("startupEarlyInputAccepted",true).put("startupEarlyInputBeforeAppReady",!ready && !after.getBoolean("readyForApps"));
                result.put("startupInput",new JSONObject().put("keyCode",KeyEvent.KEYCODE_HOME)
                    .put("acceptedBefore",before).put("acceptedAfter",session(false).getJSONObject("input").getLong("keyEventsAccepted"))
                    .put("stateBeforeSend",state).put("stateAfterAccepted",after).put("source","GuestInput32.injectInputEventReply"));
            }
            result.put("startupReadinessQueries",queries).put("startupOwnerPid",owner);
            if(ready){result.put("appsReady",true);return true;}
            check(!state.getBoolean("credentialRequired"),"Unlock Android before validating apps");
            check(SystemClock.elapsedRealtime()<deadline,"Actual app startup readiness timed out");Thread.sleep(250);
        }
    }
    private String install(Map<String,Uri> files,JSONObject expected)throws Exception {
        return install(files,expected,false);
    }
    private String install(Map<String,Uri> files,JSONObject expected,boolean pending)throws Exception {
        JSONObject before=null;long acceptedBefore=0,acceptedAt=0;
        if(pending) {
            check(GuestManager32.selected(root),"Pending install requires owner control");
            Bitmap bitmap=ui(()->runtime.snapshot());check(bitmap!=null,"Missing pending-install geometry");
            int width=bitmap.getWidth(),height=bitmap.getHeight();bitmap.recycle();
            before=new JSONObject(GuestManager32.execute(root,width,height,java.util.concurrent.ConcurrentHashMap.newKeySet(),
                ()->false,new File(root,"logs/pending-install-startup.log"),new String[]{"startup"}));
            result.put("pendingInstallBefore",before);
            check(!before.getBoolean("readyForApps") && !before.getBoolean("credentialRequired"),"Install must begin during automatic startup");
            acceptedBefore=session(false).getJSONObject("input").getLong("keyEventsAccepted");
        }
        ui(()->{runtime.installApps(files);return null;});long deadline=SystemClock.elapsedRealtime()+330000;
        while(true) {
            String status=ui(()->runtime.status());if(status.equals("Installed "+expected.getString("package")))break;
            check(!status.contains("operation failed") && SystemClock.elapsedRealtime()<deadline,"App install: "+status);Thread.sleep(250);
            if(pending && acceptedAt==0 && status.startsWith("Installing APKs in Android 12L")) {
                Thread.sleep(1000);ui(()->{runtime.key(KeyEvent.KEYCODE_HOME);return null;});
                long inputDeadline=SystemClock.elapsedRealtime()+5000;
                while(session(false).getJSONObject("input").getLong("keyEventsAccepted")<acceptedBefore+4) {
                    check(SystemClock.elapsedRealtime()<inputDeadline,"Input stalled during pending install");Thread.sleep(50);
                }
                acceptedAt=SystemClock.uptimeMillis();
            }
        }
        String log=new String(Files.readAllBytes(new File(root,"logs/manager-operation.log").toPath()),"UTF-8");
        check(log.contains("POCKET_MANAGER32_INSTALL_READY "),"Install did not wait for package broadcast delivery");
        if(pending) {
            JSONObject wait=null,ready=null;
            for(String line:log.split("\n")) {
                if(line.startsWith("POCKET_APP_READINESS32_WAIT ")){check(wait==null,"Duplicate startup wait receipt");wait=new JSONObject(line.substring("POCKET_APP_READINESS32_WAIT ".length()));}
                if(line.startsWith("POCKET_APP_READINESS32_READY ")){check(ready==null,"Duplicate startup ready receipt");ready=new JSONObject(line.substring("POCKET_APP_READINESS32_READY ".length()));}
            }
            check(wait!=null && ready!=null && !wait.getBoolean("readyForApps") && !wait.getBoolean("credentialRequired")
                && ready.getBoolean("readyForApps") && ready.getBoolean("bootBroadcastComplete") && ready.getBoolean("userUnlocked")
                && ready.getBoolean("primaryVolumeMounted") && wait.getInt("pid")==before.getInt("pid")
                && ready.getInt("pid")==before.getInt("pid"),"Missing actual install wait-to-ready transition");
            check(acceptedAt>=wait.getLong("uptimeMillis") && acceptedAt<ready.getLong("uptimeMillis"),"Input was not accepted while install waited for readiness");
            result.put("pendingInstall",new JSONObject().put("wait",wait).put("ready",ready).put("inputAcceptedUptimeMillis",acceptedAt)
                .put("keysAcceptedBefore",acceptedBefore).put("keysAcceptedAfter",session(false).getJSONObject("input").getLong("keyEventsAccepted")));
            result.put("pendingAppOperationWaitVerified",true);
        }
        return log;
    }
    private JSONObject mediaPermissions()throws Exception {
        check(spec.getString("package").equals("dev.kreijstal.pocketav32test"),"Permission fixture package mismatch");
        String shell=". /system/etc/pocket-runtime-env.sh; export LD_PRELOAD=/system/lib64/libpocket_binder_api32.so; exec /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /tmp/manager-app-inspector.apk /system/bin dev.kreijstal.aosppocket.guest.MediaPermissions32";
        ProcessBuilder builder=new ProcessBuilder(SyscallBackend.command(root,"/",Arrays.asList("/system/bin/sh","-c",shell))).directory(root);
        builder.environment().clear();builder.environment().putAll(SyscallBackend.environment(root,android.os.Process.myPid(),32));
        File log=new File(root,"logs/av-permissions.log");builder.redirectErrorStream(true).redirectOutput(log);java.lang.Process process=builder.start();
        try {
            check(process.waitFor(60,TimeUnit.SECONDS),"AV permission grant timed out");check(log.length()<65536,"AV permission log exceeds limit");
            String text=new String(Files.readAllBytes(log.toPath()),"UTF-8");check(process.exitValue()==0,"AV permission grant failed: "+text);
            for(String line:text.split("\n"))if(line.startsWith("POCKET_AV_PERMISSIONS32 ")) {
                JSONObject proof=new JSONObject(line.substring("POCKET_AV_PERMISSIONS32 ".length()));
                check(proof.getString("validation").equals("PASS") && proof.getString("package").equals(spec.getString("package"))
                    && proof.getInt("grantUid")==1000 && proof.getString("via").equals("PackageManager.grantRuntimePermission"),"Unexpected AV permission grant");return proof;
            }
            throw new IOException("No AV permission receipt");
        }finally{if(process.isAlive())process.destroyForcibly();}
    }
    private void checkEmbeddedManagerControl()throws Exception {
        if(!GuestManager32.selected(root))return;
        JSONObject checks=new JSONObject();
        String shell=". /system/etc/pocket-runtime-env.sh; export LD_PRELOAD=/system/lib64/libpocket_binder_api32.so; exec /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /tmp/manager-app-inspector.apk /system/bin dev.kreijstal.aosppocket.guest.AppInspector32 --manager-peer-denied";
        ProcessBuilder builder=new ProcessBuilder(SyscallBackend.command(root,"/",Arrays.asList("/system/bin/sh","-c",shell))).directory(root);
        builder.environment().clear();builder.environment().putAll(SyscallBackend.environment(root,android.os.Process.myPid(),32));
        File denied=new File(root,"logs/manager-peer-denied.log");builder.redirectErrorStream(true).redirectOutput(denied);java.lang.Process process=builder.start();
        try {
            check(process.waitFor(60,TimeUnit.SECONDS) && process.exitValue()==0,"Guest control peer denial probe failed");
            String text=new String(Files.readAllBytes(denied.toPath()),"UTF-8");boolean found=false;
            for(String line:text.split("\n"))if(line.startsWith("POCKET_MANAGER32_PEER_DENIED ")) {
                JSONObject peer=new JSONObject(line.substring("POCKET_MANAGER32_PEER_DENIED ".length()));
                check(peer.getBoolean("denied") && peer.getInt("pid")>0 && peer.getInt("uid")==0,"Invalid peer denial receipt");
                checks.put("guestPeerPid",peer.getInt("pid")).put("guestPeerDenied",true);found=true;
            }
            check(found,"Missing guest peer denial receipt");
        }finally{if(process.isAlive())process.destroyForcibly();}
        Bitmap bitmap=ui(()->runtime.snapshot());check(bitmap!=null,"No geometry for control validation");
        int width=bitmap.getWidth(),height=bitmap.getHeight();bitmap.recycle();
        for(String kind:new String[]{"unknown","install"}) {
            String[] args=kind.equals("unknown")?new String[]{"invalid-manager-operation"}:new String[]{"install","unexpected"};
            String expected=kind.equals("unknown")?"Unknown Manager operation":"Unexpected install argument";
            File log=new File(root,"logs/manager-negative-"+kind+".log");boolean rejected=false;
            try{GuestManager32.execute(root,width,height,java.util.concurrent.ConcurrentHashMap.newKeySet(),()->false,log,args);}
            catch(IOException error){rejected=log.isFile() && new String(Files.readAllBytes(log.toPath()),"UTF-8").contains(expected);}
            check(rejected,"Invalid owner request was not rejected: "+kind);
            checks.put(kind.equals("unknown")?"unknownOperationRejected":"badInstallArgumentsRejected",true);
        }
        result.put("managerControlChecks",checks);
    }
    private void ownerAction(String action)throws Exception {
        File request=new File(root,"tmp/pocket-volume-owner-action");
        String command=(spec.optBoolean("requireKeyCanary")?"key-":"")+action+" "+runId;
        if(storageOwnerRequest==null) {
            check(action.equals("seed"),"Missing owned canary request");
            Files.write(request.toPath(),command.getBytes("US-ASCII"),StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE);storageOwnerRequest=request;
        }else {
            check(new String(Files.readAllBytes(request.toPath()),"US-ASCII").endsWith(" "+runId),"Canary request ownership changed");
            Files.write(request.toPath(),command.getBytes("US-ASCII"),StandardOpenOption.TRUNCATE_EXISTING,StandardOpenOption.WRITE);
        }
    }
    private void openOwner()throws Exception {
        openPackage(spec.getJSONObject("storageOwner").getString("package"));
    }
    private void openPackage(String name)throws Exception {
        check(name.matches("[a-zA-Z][a-zA-Z0-9_]*(?:\\.[a-zA-Z0-9_]+)+"),"Invalid app package");
        ProcessBuilder builder=new ProcessBuilder(SyscallBackend.command(root,"/",Arrays.asList("/system/bin/pocket-manager-api32","open",name))).directory(root);
        builder.environment().clear();builder.environment().putAll(SyscallBackend.environment(root,android.os.Process.myPid(),32));
        File log=new File(root,"logs/storage-owner-open.log");builder.redirectErrorStream(true).redirectOutput(log);java.lang.Process process=builder.start();
        try {
            check(process.waitFor(60,TimeUnit.SECONDS),"Owner open timed out");check(log.length()<65536,"Owner open log exceeds limit");
            String text=new String(Files.readAllBytes(log.toPath()),"UTF-8");check(process.exitValue()==0 && text.contains("POCKET_MANAGER32_RESULT "),"Owner open failed: "+text);
        }finally{if(process.isAlive())process.destroyForcibly();}
    }
    private JSONObject ownerProof(String action)throws Exception {
        JSONObject expected=spec.getJSONObject("storageOwner");long deadline=SystemClock.elapsedRealtime()+60000;
        while(true) {
            JSONObject state=inspect(expected.getString("package")),proof=state.optJSONObject("externalVolumeOwner");
            if(proof!=null && proof.optString("nonce").equals(runId) && proof.optString("action").equals(action)
                    && !proof.optString("validation").equals("RUNNING")) {
                check(state.getInt("uid")>=10000 && state.getInt("minSdk")==32 && state.getInt("targetSdk")==32
                    && state.getLong("versionCode")==expected.getLong("versionCode") && state.getInt("splitCount")==0
                    && state.getString("certificateSha256").equals(expected.getString("certificateSha256")),"Canary owner package differs from pinned APK");
                check(proof.optString("validation").equals("PASS") && proof.getInt("uid")==state.getInt("uid")
                    && proof.getInt("sdk")==32 && proof.getString("package").equals(expected.getString("package")),"Owner workload failed: "+proof);
                if(spec.optBoolean("requireKeyCanary")) {
                    JSONObject key=proof.getJSONObject("keyCanary");
                    check(key.getBoolean("cryptoValid") && key.getBoolean("nonExportable")
                        && key.getString("alias").equals("pocket-volume-crash-"+runId),"Ordinary Keystore canary failed");
                    if(!action.equals("seed"))check(key.getString("cipherSha256").equals(ownerSeed.getJSONObject("externalVolumeOwner")
                        .getJSONObject("keyCanary").getString("cipherSha256")),"Original ciphertext/key changed");
                    if(action.equals("cleanup"))check(key.getBoolean("removed"),"Own test key remains after cleanup");
                }
                if(action.equals("unmounted")) {
                    check(!state.optBoolean("primaryEmulatedMounted",true) && proof.getString("storageState").equals("unmounted")
                        && proof.optBoolean("newProcessNoHeldDescriptor")
                        && proof.getInt("pid")!=ownerSeed.getJSONObject("externalVolumeOwner").getInt("pid"),"Unmounted owner state is incomplete");return state;
                }
                JSONArray files=proof.getJSONArray("files");check(files.length()==2,"Missing private canaries");
                for(int i=0;i<2;i++) {
                    JSONObject file=files.getJSONObject(i);String area=i==0?"data":"obb";
                    String path="/storage/emulated/0/Android/"+area+"/"+expected.getString("package")+(i==0?"/files":"")+"/pocket-canary-"+runId;
                    check(file.getString("area").equals(area) && file.getString("path").equals(path),"Unexpected owner canary path");
                    if(action.equals("cleanup")){check(file.optBoolean("removed"),"Owner did not remove canary");continue;}
                    check(file.optBoolean("ownReadVerified") && file.getLong("bytes")>0 && file.getLong("inode")>0,"Owner canary not independently readable");
                    if(action.equals("verify") || action.equals("hold") || action.equals("remounted")) {
                        JSONObject seed=ownerSeed.getJSONObject("externalVolumeOwner").getJSONArray("files").getJSONObject(i);
                        check(file.getLong("inode")==seed.getLong("inode")
                            && file.getLong("bytes")==seed.getLong("bytes"),"Canary inode changed during outsider checks");
                        check(action.equals("remounted")?file.getLong("device")>0:file.getLong("device")==seed.getLong("device"),"Unexpected volume device identity");
                    }
                }
                if(action.equals("hold"))check(proof.optBoolean("heldFdOpen"),"Owner did not hold its real descriptor");
                if(action.equals("remounted"))check(proof.optBoolean("newProcessNoHeldDescriptor")
                    && proof.getInt("pid")!=ownerSeed.getJSONObject("externalVolumeOwner").getInt("pid"),"Old descriptor owner crossed a volume epoch");
                return state;
            }
            check(SystemClock.elapsedRealtime()<deadline,"Owner workload returned no current "+action+" report: "+state.optString("externalVolumeOwnerError"));Thread.sleep(250);
        }
    }
    private JSONObject volumeControl(String operation)throws Exception {
        check(operation.equals("mount") || operation.equals("unmount") || operation.equals("state") || operation.equals("reset"),"Invalid volume operation");
        String shell=". /system/etc/pocket-runtime-env.sh; export LD_PRELOAD=/system/lib64/libpocket_binder_api32.so; exec /system/bin/pocket-app-process-api32 -Xnorelocate -Xnoimage-dex2oat -Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art -cp /tmp/manager-app-inspector.apk /system/bin dev.kreijstal.aosppocket.guest.VolumeControl32 "+operation+" "+runId;
        ProcessBuilder builder=new ProcessBuilder(SyscallBackend.command(root,"/",Arrays.asList("/system/bin/sh","-c",shell))).directory(root);
        builder.environment().clear();builder.environment().putAll(SyscallBackend.environment(root,android.os.Process.myPid(),32));
        File log=new File(root,"logs/volume-control-"+operation+".log");builder.redirectErrorStream(true).redirectOutput(log);java.lang.Process process=builder.start();
        try {
            boolean exited=process.waitFor(120,TimeUnit.SECONDS);check(log.length()<65536,"Volume control log exceeds limit");
            String text=new String(Files.readAllBytes(log.toPath()),"UTF-8");
            result.put("volumeControlClient_"+operation,new JSONObject().put("exited",exited)
                .put("exitCode",exited?process.exitValue():-1).put("log",text));
            check(exited && process.exitValue()==0,"Framework volume control failed: exit="+(exited?process.exitValue():"running")+" "+text);
            for(String line:text.split("\n"))if(line.startsWith("POCKET_VOLUME_CONTROL32 ")) {
                JSONObject value=new JSONObject(line.substring("POCKET_VOLUME_CONTROL32 ".length()));
                check(value.getString("nonce").equals(runId) && value.getString("operation").equals(operation)
                    && value.getString("via").equals("IStorageManager") && value.getInt("sdk")==32 && value.getInt("uid")==1000
                    && value.getString("validation").equals("PASS"),"Unexpected framework volume report");return value;
            }
            throw new IOException("Framework volume control returned no result");
        }finally{if(process.isAlive())process.destroyForcibly();}
    }
    private void volumeLifecycle()throws Exception {
        check(ownerSeed!=null && storagePhaseRequest!=null,"Volume lifecycle requires registered owner and phase request");
        JSONObject lifecycle=new JSONObject();result.put("volumeLifecycle",lifecycle);
        stage("holding-volume-descriptor");ownerAction("hold");openOwner();lifecycle.put("heldOwner",ownerProof("hold"));
        lifecycle.put("before",session(false).getJSONObject("externalStorage"));
        boolean resetting=spec.optBoolean("volumeReset");
        unmountAttempted=true;
        if(resetting) {
            stage("resetting-framework-volume");lifecycle.put("reset",volumeControl("reset"));
            lifecycle.put("reattached",session(false).getJSONObject("externalStorage"));
        }else {
            stage("unmounting-volume");lifecycle.put("unmount",volumeControl("unmount"));volumeNeedsRemount=true;
            lifecycle.put("detached",session(false).getJSONObject("externalStorage"));
        }
        int oldPid=ownerSeed.getJSONObject("externalVolumeOwner").getInt("pid");
        JSONArray retired=lifecycle.getJSONObject(resetting?"reattached":"detached").getJSONArray("retiredHolders");boolean retiredOwner=false;
        for(int i=0;i<retired.length();i++)if(retired.getInt(i)==oldPid)retiredOwner=true;
        boolean oldAlive=HostDns.trustedPeer(root,oldPid,ownerSeed.getInt("uid"));lifecycle.put("oldOwnerAlive",oldAlive);
        check(retiredOwner && !oldAlive,"The real held-descriptor owner was not retired by unmount");
        if(!resetting) {
            stage("checking-unmounted-volume");ownerAction("unmounted");openOwner();lifecycle.put("unmountedOwner",ownerProof("unmounted"));
            stage("remounting-volume");lifecycle.put("mount",volumeControl("mount"));volumeNeedsRemount=false;
            lifecycle.put("reattached",session(false).getJSONObject("externalStorage"));
        }
        long readyDeadline=SystemClock.elapsedRealtime()+30000;
        while(true) {
            JSONObject state=inspect(spec.getJSONObject("storageOwner").getString("package"));
            if(state.optBoolean("primaryEmulatedMounted") && state.optBoolean("externalPrimaryQuery")){lifecycle.put("remountReady",state);break;}
            check(SystemClock.elapsedRealtime()<readyDeadline,"Remounted framework/provider did not become queryable");Thread.sleep(250);
        }
        ownerAction("remounted");openOwner();lifecycle.put("remountedOwner",ownerProof("remounted"));
        check(new String(Files.readAllBytes(storagePhaseRequest.toPath()),"US-ASCII").equals("initial "+runId),"Storage phase request changed");
        Files.write(storagePhaseRequest.toPath(),("remounted "+runId).getBytes("US-ASCII"),StandardOpenOption.TRUNCATE_EXISTING,StandardOpenOption.WRITE);
        stage("checking-remounted-workload");openPackage(spec.getString("package"));long deadline=SystemClock.elapsedRealtime()+90000;
        while(true) {
            JSONObject state=inspect(),proof=state.optJSONObject("externalVolumeApp");
            if(proof!=null && proof.optString("nonce").equals(runId) && proof.optString("phase").equals("remounted")
                    && !proof.optString("validation").equals("RUNNING")) {
                lifecycle.put("remountedApp",state);foreground(state);
                JSONObject initial=result.getJSONObject("firstLaunch").getJSONObject("externalVolumeApp");
                check(proof.getInt("iteration")>initial.getInt("iteration") || proof.getInt("pid")!=initial.getInt("pid"),"Remount reused the old workload report");break;
            }
            check(SystemClock.elapsedRealtime()<deadline,"No fresh remounted storage workload report");Thread.sleep(250);
        }
        lifecycle.put("validation","PASS");stage("volume-lifecycle-passed");
    }
    private JSONObject inspectFinishedRecording(String previousNonce)throws Exception {
        long deadline=SystemClock.elapsedRealtime()+120000;
        for(;;) {
            JSONObject state=inspect(),proof=state.optJSONObject("mediaAvProbe");
            if(proof!=null && !proof.optString("nonce").isEmpty() && !proof.optString("nonce").equals(previousNonce)
                && proof.optString("stage").equals("complete")
                && (proof.optString("validation").equals("PASS") || proof.optString("validation").equals("FAIL")))return state;
            check(SystemClock.elapsedRealtime()<deadline,"No complete fresh Camera2/MediaRecorder workload report");Thread.sleep(1000);
        }
    }
    private void foreground(JSONObject state)throws Exception {
        String name=spec.getString("package");
        check(state.getInt("sdk")==32 && state.getString("package").equals(name) && state.getInt("uid")>=10000
            && state.getInt("minSdk")==spec.getInt("minSdk") && state.getInt("targetSdk")==spec.getInt("targetSdk")
            && state.getLong("versionCode")==spec.getLong("versionCode") && state.getInt("splitCount")==spec.getJSONArray("files").length()-1
            && state.getString("certificateSha256").equals(spec.getString("certificateSha256")),"Installed app differs from pinned public APK set");
        check(Pattern.compile("(?:mResumedActivity:|ResumedActivity:|topResumedActivity=)[^\\n]*"+Pattern.quote(name)+"/").matcher(state.getString("activities")).find(),"App is not resumed");
        // 12L includes display focus in the activity-task dump; the WM
        // "windows" subcommand only lists individual windows and surfaces.
        check(Pattern.compile("mCurrentFocus=[^\\n]*"+Pattern.quote(name)+"/").matcher(state.getString("activities")).find(),"App does not own the focused window");
        if(ExternalVolume32.selected(root))check(state.optBoolean("primaryEmulatedMounted") && state.optBoolean("externalPrimaryQuery"),"Selected external_primary volume is not mounted and queryable: "+state.optString("externalPrimaryQueryError"));
        if(spec.optString("proof").equals("media-codec-v1")) {
            JSONObject codec=state.optJSONObject("mediaCodecProbe");
            check(name.equals("dev.kreijstal.pocketcodec32test") && codec!=null && codec.optString("validation").equals("PASS"),"Ordinary MediaCodec fixture failed: "+(codec==null?state.optString("mediaCodecProbeError"):codec.toString()));
            check(codec.getInt("sdk")==32 && codec.getInt("uid")==state.getInt("uid") && codec.getString("package").equals(name),"Codec work ran under another identity");
            check(state.getJSONArray("codecRequestedPermissions").length()==0,"Codec fixture requested permissions");
        }
        if(spec.optString("proof").equals("media-av-v1")) {
            JSONObject av=state.optJSONObject("mediaAvProbe");
            // Collect both complete attempts even when one device fails. The
            // combined functional result below still requires two full passes.
            check(name.equals("dev.kreijstal.pocketav32test") && av!=null && av.optString("stage").equals("complete")
                && (av.optString("validation").equals("PASS") || av.optString("validation").equals("FAIL")),"Ordinary audio/camera fixture did not complete: "+(av==null?state.optString("mediaAvProbeError"):av.toString()));
            check(av.getInt("sdk")==32 && av.getInt("uid")==state.getInt("uid") && av.getString("package").equals(name),"AV work ran under another identity");
            Set<String> permissions=new HashSet<>();JSONArray requested=state.getJSONArray("avRequestedPermissions");for(int i=0;i<requested.length();i++)permissions.add(requested.getString(i));
            check(permissions.equals(new HashSet<>(Arrays.asList(android.Manifest.permission.CAMERA,android.Manifest.permission.RECORD_AUDIO)))
                && av.getBoolean("cameraPermission") && av.getBoolean("microphonePermission"),"AV runtime permissions differ from fixture");
        }
        if(spec.optString("proof").equals("external-volume-v1")) {
            JSONObject proof=state.optJSONObject("externalVolumeApp");
            check(proof!=null && proof.optString("validation").equals("PASS") && proof.optBoolean("cleanupComplete"),"Registered mounted-storage app failed: "+(proof==null?state.optString("externalVolumeAppError"):proof.toString()));
            check(proof.getInt("sdk")==32 && proof.getInt("uid")==state.getInt("uid"),"Storage workload ran under another identity");
            check(proof.optString("nonce").equals(runId),"Storage workload report belongs to another run");
            Set<String> passed=new HashSet<>();JSONArray checks=proof.getJSONArray("passed");for(int i=0;i<checks.length();i++)passed.add(checks.getString(i));
            check(passed.containsAll(Arrays.asList("mounted","registeredMediaStoreQuery","contentToPath","canonicalPathAndFd","providerRenameCoherence","fuseCreateAndMediaStore","fuseUnlinkCoherence","ownExternalFiles")),"Storage workload is incomplete");
            if(spec.has("storageOwner"))check(passed.contains("crossUidPrivateDenied") && proof.getJSONArray("privateDenials").length()==40
                && proof.getInt("privateOwnerUid")==ownerSeed.getInt("uid") && proof.getInt("privateOwnerUid")!=state.getInt("uid"),"Missing cross-UID private-storage denial proof");
        }
    }
    private void screenshot(String name)throws Exception {
        result.put(name+"Colors",frame(name+".png"));
        byte[] bytes=Files.readAllBytes(new File(getFilesDir(),name+".png").toPath());
        check(bytes.length<512*1024,"Screenshot exceeds report limit");
        result.put(name+"Png",android.util.Base64.encodeToString(bytes,android.util.Base64.NO_WRAP));
    }
    private void rawDnsPermissions()throws Exception {
        File client=new File(root,"tmp/dns-permission-client"),log=new File(root,"logs/dns-permissions.log");
        try {
            try(InputStream in=getAssets().open("api32-dns-client");OutputStream out=new FileOutputStream(client)){GuestImage.copy(in,out,4*1024*1024);}
            java.lang.reflect.Method patch=DirectImage.class.getDeclaredMethod("patchInterpreter",File.class,String.class);patch.setAccessible(true);
            patch.invoke(null,client,new File(root,"system/bin/pocket-loader").getPath());android.system.Os.chmod(client.getPath(),0755);
            ProcessBuilder builder=new ProcessBuilder(SyscallBackend.command(root,"/",Arrays.asList("/tmp/dns-permission-client","raw"))).directory(root);
            builder.environment().clear();builder.environment().putAll(SyscallBackend.environment(root,android.os.Process.myPid(),32));
            builder.environment().put("LD_PRELOAD","/system/lib64/libpocket_binder_api32.so");
            builder.redirectErrorStream(true).redirectOutput(log);java.lang.Process process=builder.start();
            try {
                boolean exited=process.waitFor(45,TimeUnit.SECONDS);
                check(log.length()<65536,"DNS permission log exceeds capture limit");
                String text=new String(Files.readAllBytes(log.toPath()),"UTF-8");result.put("rawDnsPermissions",text.trim());
                check(exited && process.exitValue()==0 && text.contains("API32_DNS_PACKET_RESOLVE_PASS"),"Raw DNS permission check failed: "+text);
            }finally{if(process.isAlive())process.destroyForcibly();}
        }finally{Files.deleteIfExists(client.toPath());}
    }
    private void network()throws Exception {
        stage("checking-network");
        File client=new File(root,"tmp/netd-role-client");
        try(InputStream in=getAssets().open("api32-network-client");OutputStream out=new FileOutputStream(client)){GuestImage.copy(in,out,4*1024*1024);}
        java.lang.reflect.Method patch=DirectImage.class.getDeclaredMethod("patchInterpreter",File.class,String.class);patch.setAccessible(true);
        patch.invoke(null,client,new File(root,"system/bin/pocket-loader").getPath());android.system.Os.chmod(client.getPath(),0755);
        try {
            for(String mode:new String[]{"denied","stack"}) {
                File log=new File(root,"logs/netd-role-"+mode+".log");
                ProcessBuilder builder=new ProcessBuilder(SyscallBackend.command(root,"/",Arrays.asList("/tmp/netd-role-client",mode))).directory(root);
                builder.environment().clear();builder.environment().putAll(SyscallBackend.environment(root,android.os.Process.myPid(),32));
                builder.environment().put("LD_PRELOAD","/system/lib64/libpocket_binder_api32.so");
                builder.redirectErrorStream(true).redirectOutput(log);java.lang.Process process=builder.start();
                try{check(process.waitFor(20,TimeUnit.SECONDS) && process.exitValue()==0,"Netd role check failed: "+mode);}
                finally{if(process.isAlive())process.destroyForcibly();}
                check(log.length()<65536,"Netd role log exceeds capture limit");String text=new String(Files.readAllBytes(log.toPath()),"UTF-8");
                check(text.contains(mode.equals("denied")?"API32_NETD_ROLE_DENIED_PASS":"API32_NETD_STACK_FWMARK_UNSUPPORTED_PASS"),"Missing netd role proof");
                result.put("netdRole_"+mode,text.trim());
            }
        }finally{Files.deleteIfExists(client.toPath());}
        long deadline=SystemClock.elapsedRealtime()+60000;
        while(true) {
            // The embedded bridge belongs to system_server and writes through
            // liblog. properties.log contains only the temporary pre-init
            // property service, which is stopped before framework startup.
            String text=nativeLog();
            check(text.length()<2*1024*1024,"Service log exceeds capture limit");
            if(text.contains("API32_HOST_NETWORK_REGISTERED "))break;
            result.put("networkStartupTail",text.substring(Math.max(0,text.length()-8000)));
            check(SystemClock.elapsedRealtime()<deadline,"Host-uplink NetworkAgent did not register");Thread.sleep(250);
        }
        File report=new File(root,"logs/host-bridge-check.json");
        JSONArray attempts=new JSONArray();result.put("networkAttempts",attempts);
        deadline=SystemClock.elapsedRealtime()+60000;
        while(true) {
            Files.deleteIfExists(report.toPath());ui(()->{runtime.checkHostBridges();return null;});
            JSONObject state=null;
            while(state==null) {
                String status=ui(()->runtime.status());
                check(!status.contains("operation failed") && SystemClock.elapsedRealtime()<deadline,"Manager network check: "+status);
                if(report.isFile() && report.length()>0) {
                    check(report.length()<2*1024*1024,"Network report exceeds capture limit");
                    try{state=new JSONObject(new String(Files.readAllBytes(report.toPath()),"UTF-8"));}
                    catch(JSONException incomplete){/* The producer may still be writing. */}
                }
                if(state==null)Thread.sleep(250);
            }
            result.put("network",state);attempts.put(new JSONObject().put("defaultNetwork",state.opt("guest_default_network"))
                .put("connected",state.optBoolean("guest_network_connected")).put("httpStatus",state.optInt("http_status",-1))
                .put("httpError",state.opt("http_error")));
            JSONObject dns=state.optJSONObject("guest_async_dns");
            if(state.optBoolean("guest_network_connected") && !state.isNull("guest_default_network")
                && state.optBoolean("guest_internet") && state.optBoolean("guest_validated") && state.optBoolean("http_via_guest_network") && state.optInt("http_status")==200
                && dns!=null && dns.optInt("uid")>=10000 && dns.optBoolean("publicAsyncApi") && dns.optInt("negativeRcode")==3) {
                rawDnsPermissions();return;
            }
            check(SystemClock.elapsedRealtime()<deadline,"Guest default-network API or network-bound HTTPS failed");Thread.sleep(1000);
        }
    }
    private void readFailedTombstones() {
        try {
            String predecessor=getIntent().getStringExtra("predecessor_run_id");
            String expectedLog=getIntent().getStringExtra("predecessor_native_log_sha256");
            check(predecessor!=null && predecessor.matches("[a-f0-9]{32}") && !predecessor.equals(runId)
                && expectedLog!=null && expectedLog.matches("[a-f0-9]{64}"),"Invalid stopped-run artifact request");
            File saved=new File(getFilesDir(),"app32-result.json");
            check(saved.isFile() && !Files.isSymbolicLink(saved.toPath()) && saved.length()<8*1024*1024,"Missing bounded prior app report");
            JSONObject prior=new JSONObject(new String(Files.readAllBytes(saved.toPath()),"UTF-8"));
            check(prior.getString("runId").equals(predecessor) && prior.getString("validation").equals("FAIL"),"Expected the exact failed predecessor");
            String log=prior.getString("nativeLog");
            byte[] logHash=MessageDigest.getInstance("SHA-256").digest(log.getBytes("UTF-8"));
            StringBuilder actualLog=new StringBuilder();for(byte value:logHash)actualLog.append(String.format(Locale.ROOT,"%02x",value&255));
            check(actualLog.toString().equals(expectedLog),"Prior native log differs");
            JSONObject stopped=prior.getJSONObject("stoppedSession");
            check(stopped.getInt("ownerExitCode")==0 && !stopped.getBoolean("ownerAlive") && !stopped.has("cleanupFailure")
                && DirectImage.leftovers(root).isEmpty(),"Prior guest did not stop cleanly");
            File directory=new File(root,"system-data/tombstones");
            check(directory.isDirectory() && !Files.isSymbolicLink(directory.toPath())
                && directory.getCanonicalFile().equals(directory.getAbsoluteFile()),"Unexpected tombstone directory");
            Set<String> names=new LinkedHashSet<>();
            Matcher markers=Pattern.compile("(?m)^tombstoned:Tombstone written to: (tombstone_[0-9]+)$").matcher(log);
            while(markers.find())names.add(markers.group(1));
            check(!names.isEmpty() && names.size()<=4,"No bounded tombstones recorded in the failed run");
            JSONArray artifacts=new JSONArray();
            for(String name:names) {
                File file=new File(directory,name);
                java.io.FileDescriptor descriptor=android.system.Os.open(file.getPath(),android.system.OsConstants.O_RDONLY
                    |android.system.OsConstants.O_CLOEXEC|android.system.OsConstants.O_NOFOLLOW,0);
                try(FileInputStream input=new FileInputStream(descriptor)) {
                    android.system.StructStat before=android.system.Os.fstat(descriptor);
                    check((before.st_mode&android.system.OsConstants.S_IFMT)==android.system.OsConstants.S_IFREG
                        && before.st_size>0 && before.st_size<=1024*1024,"Tombstone exceeds capture bound");
                    ByteArrayOutputStream bytes=new ByteArrayOutputStream();byte[] buffer=new byte[8192];int count;
                    while((count=input.read(buffer))!=-1){check(bytes.size()+count<=1024*1024,"Tombstone grew beyond capture bound");bytes.write(buffer,0,count);}
                    check(bytes.size()==before.st_size && android.system.Os.fstat(descriptor).st_size==before.st_size,"Tombstone changed during capture");
                    artifacts.put(new JSONObject().put("name",name).put("bytes",bytes.size()).put("text",bytes.toString("UTF-8")));
                }
            }
            result.put("runId",runId).put("probeKind","stopped-failure-tombstones").put("predecessorRunId",predecessor)
                .put("predecessorNativeLogSha256",expectedLog).put("guestStarted",false).put("priorReportPreserved",true)
                .put("tombstones",artifacts).put("validation","PASS");
        }catch(Exception error){try{result.put("validation","FAIL").put("error",error.toString());}catch(Exception ignored){}}
        try{stage("finished");finished=result.toString();}catch(Exception ignored){}
    }
    @Override protected void probe() {
        if(readTombstonesOnly){readFailedTombstones();return;}
        try {
            try(InputStream in=getAssets().open("api32-app-spec.json")) {
                ByteArrayOutputStream bytes=new ByteArrayOutputStream();GuestImage.copy(in,bytes,64*1024);spec=new JSONObject(bytes.toString("UTF-8"));
            }
            check(spec.getString("package").matches("[a-zA-Z][a-zA-Z0-9_]*(?:\\.[a-zA-Z0-9_]+)+"),"Invalid app package");
            boolean policyOnly=getIntent().getBooleanExtra("observe_activity_policy_only",false);
            boolean cryptoOnly=getIntent().getBooleanExtra("observe_crypto_frontend_only",false);
            boolean startupOnly=getIntent().getBooleanExtra("observe_startup_readiness_only",false);
            check((policyOnly?1:0)+(cryptoOnly?1:0)+(startupOnly?1:0)<=1,"Select one diagnostic");
            boolean pendingInstall=getIntent().getBooleanExtra("require_pending_install",false);
            check(!pendingInstall || (!policyOnly && !cryptoOnly && !startupOnly && spec.optString("proof").equals("media-av-v1")
                && !spec.has("storageOwner") && !spec.optBoolean("volumeLifecycle") && !spec.optBoolean("requireLateCameraGrant")),"Pending install requires the complete AV fixture");
            Map<String,Uri> preloaded=pendingInstall?download():null;
            result.put("runId",runId).put("sdk",32).put("appSpec",spec).put("diagnosticOnly",policyOnly || cryptoOnly || startupOnly);
            if(spec.optBoolean("managerCrashRecovery"))result.put("managerCrashResume",crashResume).put("managerHostPid",android.os.Process.myPid());
            stage("booting");boot();
            if(pendingInstall){stage("installing-during-startup");result.put("installOperation",install(preloaded,spec,true));result.put("installed",true);}
            awaitAppReadiness(startupOnly);
            if(crashResume)verifyCrashRestart();
            checkInstalldFrontend();
            if(!policyOnly && !cryptoOnly && !startupOnly)checkCrashTransport();
            if(!policyOnly && !cryptoOnly && !startupOnly && spec.optBoolean("requireLateCameraGrant"))awaitHostCameraGrant();
            File imageStats=new File(root,".pocket-image-install-stats.json");
            if(imageStats.isFile()){check(imageStats.length()<8192,"Image install statistics exceed limit");result.put("imageInstallStats",new JSONObject(new String(Files.readAllBytes(imageStats.toPath()),"UTF-8")));}
            if(startupOnly) {
                result.put("homeColors",result.getInt("startupHomeColors"));
                result.put("cryptoFrontend",cryptoFrontend());
                result.put("display",new JSONObject(new String(Files.readAllBytes(new File(root,"logs/display-performance.json").toPath()),"UTF-8")));
                result.put("functionalPassed",true).put("scope","Same-process system-server boot/user/mounted-volume readiness, early input and temporary Gatekeeper credential lifecycle; no install, media, account or locked-CE credential claim");return;
            }
            if(cryptoOnly) {
                stage("checking-shared-crypto-frontend");result.put("homeColors",frame("crypto-frontend-home.png"));
                if(ExternalVolume32.selected(root)) {
                    stage("waiting-crypto-external-volume");long until=SystemClock.elapsedRealtime()+180000;
                    while(!console().contains("API32_EXTERNAL_VOLUME_MOUNT result=0 ")) {
                        check(SystemClock.elapsedRealtime()<until,"External volume did not finish mounting before credential validation");
                        Thread.sleep(250);
                    }
                    result.put("externalVolumeReadyBeforeCrypto",true);
                }
                result.put("cryptoFrontend",cryptoFrontend());
                result.put("display",new JSONObject(new String(Files.readAllBytes(new File(root,"logs/display-performance.json").toPath()),"UTF-8")));
                result.put("functionalPassed",true).put("scope","Shared UID1000 Gatekeeper native enrollment, wrong-password rejection, re-enrollment, challenge/token/SID lifecycle and ordinary-role denial; no user0 credential changes or CE/media compatibility claim");return;
            }
            if(policyOnly) {
                stage("observing-activity-policy");result.put("homeColors",frame("activity-policy-home.png"));
                Files.copy(new File(getApplicationInfo().sourceDir).toPath(),new File(root,"tmp/manager-app-inspector.apk").toPath(),StandardCopyOption.REPLACE_EXISTING);
                JSONArray observations=new JSONArray();result.put("activityPolicySnapshots",observations);
                for(int i=0;i<2;i++) {
                    if(i!=0)Thread.sleep(20000);
                    JSONObject state=inspect("--activity-policy");
                    check(state.getInt("sdk")==32 && state.getInt("uid")==1000 && state.getString("diagnostic").equals("activity-policy"),"Wrong policy observer identity");
                    observations.put(state.put("phase",i==0?"booted":"settled"));progress=result.toString();
                }
                result.put("display",new JSONObject(new String(Files.readAllBytes(new File(root,"logs/display-performance.json").toPath()),"UTF-8")));
                result.put("functionalPassed",true).put("scope","Read-only live guest ActivityManager policy/process snapshots after normal Manager boot and settling; no app install, foreground lifecycle, network or media workload claim");return;
            }
            if(getIntent().getBooleanExtra("require_network",false))network();
            result.put("homeColors",frame("app32-home.png"));
            Files.copy(new File(getApplicationInfo().sourceDir).toPath(),new File(root,"tmp/manager-app-inspector.apk").toPath(),StandardCopyOption.REPLACE_EXISTING);
            checkEmbeddedManagerControl();
            if(spec.optString("proof").equals("external-volume-v1")) {
                File request=new File(root,"tmp/pocket-volume-app-run");
                if(crashResume)check(new String(Files.readAllBytes(request.toPath()),"US-ASCII").equals(runId),"Crash request nonce changed");
                else Files.write(request.toPath(),runId.getBytes("US-ASCII"),StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE);
                storageRequest=request;
            }
            if(spec.optBoolean("volumeLifecycle")) {
                check(storageRequest!=null && spec.has("storageOwner"),"Invalid volume lifecycle specification");
                File request=new File(root,"tmp/pocket-volume-app-phase");
                Files.write(request.toPath(),("initial "+runId).getBytes("US-ASCII"),StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE);storagePhaseRequest=request;
            }
            if(spec.has("storageOwner")) {
                JSONObject owner=spec.getJSONObject("storageOwner");check(spec.optString("proof").equals("external-volume-v1")
                    && owner.getString("package").equals("dev.kreijstal.pocketvolumeowner32"),"Invalid storage owner fixture");
                if(crashResume) {
                    JSONObject previous=crashCheckpoint.getJSONObject("body");ownerSeed=previous.getJSONObject("storageOwnerBefore");
                    storageOwnerRequest=new File(root,"tmp/pocket-volume-owner-action");
                    ownerAction("remounted");openOwner();JSONObject resumed=ownerProof("remounted");
                    result.put("crashOriginalOwner",ownerSeed).put("crashRemountedOwner",resumed);
                    ownerSeed=resumed;result.put("storageOwnerBefore",resumed).put("storageOwnerInstall",previous.getString("storageOwnerInstall"));
                }else {
                    ownerAction("seed");stage("installing-storage-owner");
                    result.put("storageOwnerInstall",install(download(owner,"compat32-owner"),owner));
                    ownerSeed=ownerProof("seed");result.put("storageOwnerBefore",ownerSeed);
                }
            }
            if(crashResume) {
                stage("opening-retained-app-after-crash");openPackage(spec.getString("package"));
                result.put("installOperation",crashCheckpoint.getJSONObject("body").getString("installOperation")).put("noInstallAfterCrash",true);
            }else if(!pendingInstall){stage("downloading-app");Map<String,Uri> files=download();stage("installing");result.put("installOperation",install(files,spec));}
            if(spec.optString("proof").equals("media-av-v1")){stage("granting-media-permissions");result.put("mediaPermissions",mediaPermissions());}
            result.put("installed",true);stage("observing-first-launch");Thread.sleep(30000);
            JSONObject first=spec.optBoolean("requireCameraRecorder")?inspectFinishedRecording(null):inspect();result.put("firstLaunch",first);screenshot("app32-first");foreground(first);
            ui(()->{runtime.key(KeyEvent.KEYCODE_HOME);return null;});Thread.sleep(1000);
            stage("checking-home");long homeDeadline=SystemClock.elapsedRealtime()+30000;
            String homePackage=first.getString("homePackage");check(!homePackage.isEmpty(),"No default launcher resolved");
            while(true) {
                JSONObject home=inspect();String activities=home.getString("activities");
                if(Pattern.compile("(?:mResumedActivity:|ResumedActivity:|topResumedActivity=)[^\\n]*"+Pattern.quote(homePackage)+"/").matcher(activities).find()
                    && Pattern.compile("mCurrentFocus=[^\\n]*"+Pattern.quote(homePackage)+"/").matcher(activities).find()) {
                    result.put("homeVerified",true).put("homeState",home);break;
                }
                check(SystemClock.elapsedRealtime()<homeDeadline,"Home did not focus the actual default launcher");Thread.sleep(250);
            }
            check(ui(()->runtime.hasLastApp()),"Last app preference missing");ui(()->{runtime.openLastApp();return null;});
            stage("observing-reopen");Thread.sleep(40000);
            JSONObject second=spec.optBoolean("requireCameraRecorder")?inspectFinishedRecording(first.getJSONObject("mediaAvProbe").getString("nonce")):inspect();result.put("reopened",second);screenshot("app32-reopened");foreground(second);
            if(spec.optString("proof").equals("media-codec-v1")) {
                JSONObject a=first.getJSONObject("mediaCodecProbe"),b=second.getJSONObject("mediaCodecProbe");
                check(!a.getString("nonce").equals(b.getString("nonce")) && b.getLong("startedUptimeMillis")>=a.getLong("finishedUptimeMillis"),"Codec reopen reused the previous workload result");
                result.put("codecWorkloadRepeated",true);
            }
            if(spec.optString("proof").equals("media-av-v1")) {
                JSONObject a=first.getJSONObject("mediaAvProbe"),b=second.getJSONObject("mediaAvProbe");
                check(!a.getString("nonce").equals(b.getString("nonce")) && b.getLong("startedUptimeMillis")>=a.getLong("finishedUptimeMillis"),"AV reopen reused the previous workload result");
                result.put("avWorkloadRepeated",true);
            }
            result.put("display",new JSONObject(new String(Files.readAllBytes(new File(root,"logs/display-performance.json").toPath()),"UTF-8")));
            if(ownerSeed!=null) {stage("verifying-storage-owner");ownerAction("verify");openOwner();result.put("storageOwnerAfter",ownerProof("verify"));}
            if(spec.optBoolean("volumeLifecycle"))volumeLifecycle();
            if(spec.optBoolean("managerCrashRecovery") && !crashResume)awaitManagerCrash();
            if(crashResume)result.put("managerCrashRecoveryVerified",true);
            result.put("functionalPassed",true).put("scope","Unmodified API32 split APK installation, foreground startup and Home/reopen; no account registration, messaging or calls");
            if(spec.optString("proof").equals("media-codec-v1"))result.put("scope","Ordinary SDK32 app with no requested permissions: actual AAC/AVC compression, PCM signal/YUV pixel decode checks and a fresh repeated codec workload after Home/reopen");
            if(spec.optString("proof").equals("media-av-v1"))result.put("scope","Ordinary SDK32 app using normal runtime camera/microphone permissions: public AudioTrack playback head, AudioRecord sample/timestamp reads and Camera2 YUV preview/JPEG, repeated fresh after Home/reopen");
            if(spec.optString("proof").equals("media-av-v1")) {
                boolean passed=first.getJSONObject("mediaAvProbe").optString("validation").equals("PASS") && second.getJSONObject("mediaAvProbe").optString("validation").equals("PASS");
                result.put("functionalPassed",passed);
                if(!passed)result.put("componentFailure","At least one ordinary audio/camera workload failed; inspect both attempt reports");
            }
        }catch(Throwable error){try{
            StringWriter stack=new StringWriter();error.printStackTrace(new PrintWriter(stack));
            result.put("functionalPassed",false).put("error",error.toString()).put("errorStack",stack.toString().substring(0,Math.min(16384,stack.toString().length())));
        }catch(Exception ignored){}}
        finally {
            if(unmountAttempted && !result.optBoolean("functionalPassed"))try {
                JSONObject state=volumeControl("state");result.put("volumeFailureState",state);
                if(state.getJSONObject("after").getInt("state")==0)volumeNeedsRemount=true;
            }catch(Exception error){try{result.put("volumeFailureStateError",error.toString());}catch(Exception ignored){}}
            if(volumeNeedsRemount)try{result.put("volumeRecovery",volumeControl("mount"));volumeNeedsRemount=false;}
            catch(Exception error){try{result.put("volumeRecoveryError",error.toString());}catch(Exception ignored){}}
            if(ownerSeed!=null)try {
                stage("cleaning-storage-owner");ownerAction("cleanup");openOwner();result.put("storageOwnerCleanup",ownerProof("cleanup"));
            }catch(Exception error){try{result.put("functionalPassed",false).put("storageOwnerCleanupError",error.toString());}catch(Exception ignored){}}
            try{stage("stopping");stop();result.put("stoppedSession",session(false));}
            catch(Exception error){try{result.put("functionalPassed",false).put("cleanupError",error.toString());}catch(Exception ignored){}}
            if(storageRequest!=null)try {
                check(new String(Files.readAllBytes(storageRequest.toPath()),"US-ASCII").equals(runId),"Storage request ownership changed");Files.delete(storageRequest.toPath());
            }catch(Exception error){try{result.put("functionalPassed",false).put("storageRequestCleanupError",error.toString());}catch(Exception ignored){}}
            if(storageOwnerRequest!=null)try {
                check(new String(Files.readAllBytes(storageOwnerRequest.toPath()),"US-ASCII").endsWith(" "+runId),"Canary request ownership changed");Files.delete(storageOwnerRequest.toPath());
            }catch(Exception error){try{result.put("functionalPassed",false).put("storageOwnerRequestCleanupError",error.toString());}catch(Exception ignored){}}
            if(storagePhaseRequest!=null)try {
                check(new String(Files.readAllBytes(storagePhaseRequest.toPath()),"US-ASCII").endsWith(" "+runId),"Storage phase ownership changed");Files.delete(storagePhaseRequest.toPath());
            }catch(Exception error){try{result.put("functionalPassed",false).put("storagePhaseCleanupError",error.toString());}catch(Exception ignored){}}
            try {
                JSONObject stopped=session(true);String logs=stopped.optString("nativeLog");
                result.put("nativeLog",logs).put("console",console());
                boolean clean=!logs.contains("ANR in ") && !logs.contains("FATAL EXCEPTION:") && !logs.contains("FATAL EXCEPTION IN SYSTEM PROCESS:")
                    && !logs.contains("Fatal signal ") && !logs.contains("JNI DETECTED ERROR IN APPLICATION:")
                    && !logs.contains("DEBUG:Abort message:")
                    && !Pattern.compile("(?m)^DEBUG:signal [0-9]+ \\(SIG(?:ABRT|SEGV|BUS|ILL|FPE|SYS|TRAP)\\)").matcher(logs).find()
                    && logs.length()<2*1024*1024;
                boolean stoppedClean=stopped.optInt("ownerExitCode",-1)==0 && !stopped.optBoolean("ownerAlive",true) && !stopped.has("cleanupFailure");
                result.put("validation",result.optBoolean("functionalPassed") && clean && stoppedClean?"PASS":"FAIL");
                stage("finished");finished=result.toString();Files.write(new File(getFilesDir(),"app32-result.json").toPath(),finished.getBytes("UTF-8"));
            }catch(Exception error){try{result.put("validation","FAIL").put("reportError",error.toString());finished=result.toString();}catch(Exception ignored){}}
        }
    }
}
