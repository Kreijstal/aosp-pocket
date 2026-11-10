// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import android.os.*;
import android.net.Uri;
import android.system.Os;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import org.json.*;

/** Media validation against packaged code or a reversible trial in VM73. */
public final class ManagerMediaSessionProbe32 extends ManagerSessionProbe32 {
    private final ArrayList<String> patched=new ArrayList<>();
    private final Map<String,Integer> modes=new HashMap<>();
    private final ArrayList<File> created=new ArrayList<>();
    private boolean ownsBackup;
    private boolean proxy,proxyFinished,pathProxy;
    private File mediaCanary;
    private File lazyMediaProbe;
    private File metricsProbe;
    private boolean supervisedBootstrap;
    private boolean systemServerBridge;
    private boolean imageRuntime;
    private final Map<String,String> packagedCode=new HashMap<>();
    private static void publish(File file,byte[] bytes)throws Exception {
        java.nio.file.Path temporary=new File(file.getParentFile(),file.getName()+".tmp").toPath();
        Files.write(temporary,bytes,StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE);
        Files.move(temporary,file.toPath(),StandardCopyOption.ATOMIC_MOVE);
    }
    private static final Set<String> PATCH_FILES=new HashSet<>(Arrays.asList("system/bin/pocket-loader","system/bin/pocket-init-api32","system/lib64/libpocket_zygote_api32.so","system/bin/pocket-core-api32","system/lib64/libpocket_binder_api32.so","system/lib64/libpocket_hidl_api32.so","system/lib64/libpocket_media_api32.so","system/etc/init/pocket-runtime.rc","system/etc/pocket-runtime-boot.sh","system/etc/pocket-profile.prop","product/overlay/PocketMemory/PocketMemory.apk","system/framework/pocket-zygote.jar","system/framework/pocket-tools.jar"));
    private JSONObject patchSpec()throws Exception {
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();
        try(InputStream in=getAssets().open("media-session-patches.json")){GuestImage.copy(in,bytes,16384);}
        JSONObject spec=new JSONObject(bytes.toString("UTF-8"));
        if(imageRuntime){check(spec.length()==0,"Packaged image trial cannot replace runtime code");return spec;}
        check(spec.length()>=2 && spec.length()<=PATCH_FILES.size(),"Invalid patch count");
        for(Iterator<String> keys=spec.keys();keys.hasNext();) {String name=keys.next();check(PATCH_FILES.contains(name) && spec.getString(name).matches("[a-f0-9]{64}"),"Invalid patch specification");}
        check(spec.has("system/bin/pocket-loader") && spec.has("system/lib64/libpocket_zygote_api32.so"),"Missing base code patches");return spec;
    }
    private static String hash(File file)throws Exception {
        java.security.MessageDigest digest=java.security.MessageDigest.getInstance("SHA-256");
        try(InputStream input=new FileInputStream(file)){byte[] bytes=new byte[65536];int n;while((n=input.read(bytes))!=-1)digest.update(bytes,0,n);}
        StringBuilder text=new StringBuilder();for(byte value:digest.digest())text.append(String.format(Locale.ROOT,"%02x",value&255));return text.toString();
    }
    private boolean cachedFixture(File apk)throws Exception {
        String expected=hash(apk);File[] groups=new File(root,"system-data/app").listFiles();if(groups==null)return false;
        for(File group:groups) {
            if(!group.isDirectory() || Files.isSymbolicLink(group.toPath()))continue;
            File[] packages=group.getName().startsWith("dev.kreijstal.pocketmediasession32-")?new File[]{group}:group.listFiles();if(packages==null)continue;
            for(File directory:packages)if(directory.getName().startsWith("dev.kreijstal.pocketmediasession32-") && !Files.isSymbolicLink(directory.toPath())) {
                File base=new File(directory,"base.apk");if(base.isFile() && !Files.isSymbolicLink(base.toPath()) && hash(base).equals(expected))return true;
            }
        }
        return false;
    }
    @Override public void onCreate(Bundle state){super.onCreate(state);autoStart=false;}
    private void patch()throws Exception {
        check(DirectImage.leftovers(root).isEmpty(),"An existing guest owns the profile");
        check(!CeStorage.enabled(root),"This trial requires the existing plaintext test profile");
        String imageId=new String(Files.readAllBytes(new File(root,".pocket-image-id").toPath()),"US-ASCII").trim();result.put("preservedImageId",imageId);
        ByteArrayOutputStream catalogBytes=new ByteArrayOutputStream();try(InputStream in=getAssets().open("pocket-images.json")){GuestImage.copy(in,catalogBytes,8*1024*1024);}
        JSONArray images=new JSONObject(catalogBytes.toString("UTF-8")).getJSONArray("images");String expected=null;
        for(int i=0;i<images.length();i++)if(images.getJSONObject(i).getInt("sdk")==32){check(expected==null,"Duplicate image pin");expected=images.getJSONObject(i).getString("sha256");}
        check(imageId.equals(expected),"Unexpected preserved image: "+imageId);
        JSONObject imageSpec=null;
        try(InputStream in=getAssets().open("api32-media-image.json")) {
            ByteArrayOutputStream bytes=new ByteArrayOutputStream();GuestImage.copy(in,bytes,8192);
            imageSpec=new JSONObject(bytes.toString("UTF-8"));
        }catch(FileNotFoundException absent){}
        if(imageSpec!=null) {
            check(imageSpec.getString("imageSha256").equals(imageId),"Wrong packaged image trial pin");
            imageRuntime=true;
            for(String name:PATCH_FILES)packagedCode.put(name,hash(new File(root,name)));
            result.put("packagedImageTrial",true).put("packagedCodeBefore",new JSONObject(packagedCode));
        }
        JSONObject patches=patchSpec();
        supervisedBootstrap=imageRuntime || patches.has("system/bin/pocket-init-api32");
        systemServerBridge=imageRuntime || patches.has("system/framework/pocket-zygote.jar");
        File backup=new File(root,".pocket-media-session-backup");
        if(backup.isDirectory() && new File(backup,"restored").isFile()) {
            String prior=new String(Files.readAllBytes(new File(backup,"restored").toPath()),"US-ASCII").trim();check(prior.matches("[a-f0-9]{32}"),"Invalid terminal trial marker");
            File savedImage=new File(backup,"image-id");String backupImage=savedImage.isFile()?new String(Files.readAllBytes(savedImage.toPath()),"US-ASCII").trim():"24456bff47052e1381cc4228908f084b49afa94fc9f09ad00ae281e89e9ac122";
            if(backupImage.equals(imageId)) {
                JSONObject savedModes=new JSONObject(new String(Files.readAllBytes(new File(backup,"modes.json").toPath()),"UTF-8"));
                for(Iterator<String> keys=savedModes.keys();keys.hasNext();) {String name=keys.next();check(PATCH_FILES.contains(name),"Invalid prior patch target");check(Arrays.equals(Files.readAllBytes(new File(backup,name).toPath()),Files.readAllBytes(new File(root,name).toPath())),"Prior code restoration differs");}
            }
            else result.put("archivedBackupFromImage",backupImage);
            Files.move(backup.toPath(),new File(root,".pocket-media-session-backup-"+prior).toPath());
        }
        check(!backup.exists() && backup.mkdir(),"Inspect the previous reversible trial before another boot");ownsBackup=true;
        Files.write(new File(backup,"image-id").toPath(),imageId.getBytes("US-ASCII"));
        Files.write(new File(backup,"modes.json").toPath(),new JSONObject(modes).toString().getBytes("UTF-8"));
        for(Iterator<String> keys=patches.keys();keys.hasNext();) {String name=keys.next();
            File original=new File(root,name),saved=new File(backup,name);saved.getParentFile().mkdirs();
            check(original.isFile() && !Files.isSymbolicLink(original.toPath()),"Unexpected code patch target");modes.put(name,Os.stat(original.getPath()).st_mode&07777);Files.copy(original.toPath(),saved.toPath());patched.add(name);
            Files.write(new File(backup,"modes.json").toPath(),new JSONObject(modes).toString().getBytes("UTF-8"));
            try(InputStream in=getAssets().open("media-session-patch/"+name);OutputStream out=new FileOutputStream(original)){GuestImage.copy(in,out,4*1024*1024);}
            check(hash(original).equals(patches.getString(name)),"Patch payload hash mismatch");
            if(name.equals("system/bin/pocket-core-api32") || name.equals("system/bin/pocket-init-api32")) {
                java.lang.reflect.Method patch=DirectImage.class.getDeclaredMethod("patchInterpreter",File.class,String.class);patch.setAccessible(true);patch.invoke(null,original,new File(root,"system/bin/pocket-loader").getPath());
            }
            Os.chmod(original.getPath(),modes.get(name));
        }
        result.put("guestPatchPayloadSha256",patches);
        if(systemServerBridge) {
            File marker=new File(root,"tmp/pocket-bridge-probe");check(!marker.exists(),"Bridge probe marker already exists");
            created.add(marker);Files.write(marker.toPath(),runId.getBytes("US-ASCII"));
            File control=new File(root,"dev/socket/pocket_control32");
            if(Files.exists(control.toPath(),LinkOption.NOFOLLOW_LINKS))check(android.system.OsConstants.S_ISSOCK(Os.lstat(control.getPath()).st_mode),"Unexpected existing input endpoint");
            created.add(control);
        }
        if(supervisedBootstrap)for(String name:new String[]{"tmp/pocket-runtime-services-ready","tmp/pocket-runtime-services-ready.new"}) {
            File file=new File(root,name);
            if(imageRuntime && Files.exists(file.toPath(),LinkOption.NOFOLLOW_LINKS)) {
                check(!Files.isSymbolicLink(file.toPath()) && file.isFile() && file.length()<=64,"Unexpected prior bootstrap marker");
                String prior=new String(Files.readAllBytes(file.toPath()),"US-ASCII").trim();
                check(prior.matches("[a-f0-9]{32}"),"Invalid prior boot nonce");
                // Normal boots retain their generated readiness nonce. The
                // runtime itself removes it on its next start; retire it here
                // before the negative bootstrap fixtures run.
                Files.delete(file.toPath());result.put("retiredPriorBootstrapNonce",prior);
            }
            check(!Files.exists(file.toPath(),LinkOption.NOFOLLOW_LINKS),"Bootstrap marker already exists");created.add(file);
        }
        try(InputStream in=getAssets().open("api32-lazy-media-probe")) {
            lazyMediaProbe=new File(root,"tmp/pocket-lazy-media-probe");check(!lazyMediaProbe.exists(),"Lazy-media probe already exists");created.add(lazyMediaProbe);
            try(OutputStream out=new FileOutputStream(lazyMediaProbe)){GuestImage.copy(in,out,4*1024*1024);}
            java.lang.reflect.Method patch=DirectImage.class.getDeclaredMethod("patchInterpreter",File.class,String.class);patch.setAccessible(true);patch.invoke(null,lazyMediaProbe,new File(root,"system/bin/pocket-loader").getPath());Os.chmod(lazyMediaProbe.getPath(),0755);
        }catch(FileNotFoundException absent){}
        try(InputStream in=getAssets().open("api32-metrics-probe")) {
            check(imageRuntime,"Metrics caller-policy probe requires packaged runtime code");
            metricsProbe=new File(root,"tmp/pocket-metrics-probe");check(!metricsProbe.exists(),"Metrics probe already exists");created.add(metricsProbe);
            try(OutputStream out=new FileOutputStream(metricsProbe)){GuestImage.copy(in,out,4*1024*1024);}
            java.lang.reflect.Method patch=DirectImage.class.getDeclaredMethod("patchInterpreter",File.class,String.class);patch.setAccessible(true);patch.invoke(null,metricsProbe,new File(root,"system/bin/pocket-loader").getPath());Os.chmod(metricsProbe.getPath(),0755);
        }catch(FileNotFoundException absent){}
        result.put("preexistingUpperRootSymlink",Files.isSymbolicLink(new File(root,"storage/emulated").toPath()))
            .put("preexistingUpperUserSymlink",Files.isSymbolicLink(new File(root,"storage/emulated/0").toPath()));
        for(String relative:new String[]{"dev.kreijstal.pocketmediasession32","dev.kreijstal.pocketmediasession32.other"}) {
            File canary=new File(root,"system-data/media/0/Android/data/"+relative+"/files/canary");
            check(!canary.exists(),"Media fixture canary already exists");canary.getParentFile().mkdirs();created.add(canary);Files.write(canary.toPath(),("pocket-real-media-"+runId).getBytes("UTF-8"));
        }
        File nonce=new File(root,"tmp/pocket-media-run");check(!nonce.exists(),"Media invocation nonce already exists");created.add(nonce);Files.write(nonce.toPath(),runId.getBytes("US-ASCII"));
        try(InputStream marker=getAssets().open("api32-media-proxy.json")) {proxy=true;ByteArrayOutputStream bytes=new ByteArrayOutputStream();GuestImage.copy(marker,bytes,8192);pathProxy=new JSONObject(bytes.toString("UTF-8")).optBoolean("externalPaths");}catch(FileNotFoundException absent){}
        if(proxy) {
            for(String name:new String[]{"tmp/pocket-media-proxy","tmp/media-proxy-ready","tmp/media-proxy-ready.tmp","tmp/media-proxy-done","tmp/media-proxy-done.tmp","dev/socket/pocket_ext","dev/socket/pme32"}) {
                File file=new File(root,name);check(!Files.exists(file.toPath(),LinkOption.NOFOLLOW_LINKS),"Proxy fixture path already exists");created.add(file);
            }
            Files.write(new File(root,"tmp/pocket-media-proxy").toPath(),runId.getBytes("US-ASCII"));
            if(pathProxy) {
                for(String name:new String[]{".pocket-external-enabled",".pocket-external-ready",".pocket-external-device","dev/socket/pocket_efs","dev/socket/pocket_xfd","tmp/media-proxy-host-ready","tmp/media-proxy-host-ready.tmp","tmp/media-proxy-app-done","tmp/media-proxy-app-done.tmp"}) {
                    File file=new File(root,name);check(!Files.exists(file.toPath(),LinkOption.NOFOLLOW_LINKS),"Path fixture already exists");created.add(file);
                }
                Files.write(new File(root,".pocket-external-enabled").toPath(),runId.getBytes("US-ASCII"));
            }
            mediaCanary=new File(root,"system-data/media/0/Android/media/dev.kreijstal.pocketmediasession32/canary");check(!mediaCanary.exists(),"Media data canary already exists");mediaCanary.getParentFile().mkdirs();created.add(mediaCanary);
            byte[] data=new byte[8192];Arrays.fill(data,(byte)'Q');byte[] start=("pocket-real-media-"+runId).getBytes("UTF-8");System.arraycopy(start,0,data,0,start.length);Files.write(mediaCanary.toPath(),data);
        }
        File client=new File(root,"tmp/pocket-media-client");check(!client.exists(),"Native media fixture already exists");
        created.add(client);File socket=new File(root,"dev/socket/pms32");check(!Files.exists(socket.toPath(),LinkOption.NOFOLLOW_LINKS),"Media control socket already exists");created.add(socket);
        try(InputStream in=getAssets().open("api32-media-client");OutputStream out=new FileOutputStream(client)){GuestImage.copy(in,out,4*1024*1024);}
        java.lang.reflect.Method method=DirectImage.class.getDeclaredMethod("patchInterpreter",File.class,String.class);method.setAccessible(true);method.invoke(null,client,new File(root,"system/bin/pocket-loader").getPath());Os.chmod(client.getPath(),0755);
        result.put("codePatchOnly",!imageRuntime).put("guestDataReset",false);
    }
    private void restore()throws Exception {
        if(!ownsBackup && patched.isEmpty() && created.isEmpty()){result.put("originalCodeUnchanged",true);return;}
        check(ownsBackup,"This invocation does not own the retained code backups");
        check(DirectImage.leftovers(root).isEmpty(),"Cannot restore code while guest processes remain");
        File backup=new File(root,".pocket-media-session-backup");
        for(String name:patched) {
            File saved=new File(backup,name);Files.copy(saved.toPath(),new File(root,name).toPath(),StandardCopyOption.REPLACE_EXISTING);Os.chmod(new File(root,name).getPath(),modes.get(name));
            check(Arrays.equals(Files.readAllBytes(saved.toPath()),Files.readAllBytes(new File(root,name).toPath())),"Code restoration differs");
        }
        for(File file:created)Files.deleteIfExists(file.toPath());
        if(imageRuntime) {
            Map<String,String> after=new HashMap<>();
            for(String name:packagedCode.keySet())after.put(name,hash(new File(root,name)));
            check(packagedCode.equals(after),"Packaged runtime code changed during media validation");
            result.put("packagedCodeAfter",new JSONObject(after)).put("originalCodeUnchanged",true);
        }
        // Keep byte-for-byte backups as evidence; this terminal marker permits deliberate recovery.
        if(backup.isDirectory())Files.write(new File(backup,"restored").toPath(),runId.getBytes("US-ASCII"));
        result.put("originalCodeRestored",true);
    }
    private void proxy()throws Exception {
        File ready=new File(root,"tmp/media-proxy-ready");check(ready.length()<8192,"Proxy capability bound");JSONObject info=new JSONObject(new String(Files.readAllBytes(ready.toPath()),"UTF-8"));check(info.getString("runId").equals(runId),"Wrong proxy actor nonce");
        try(ExternalFiles32 external=new ExternalFiles32(root,info.getInt("pid"))) {
            try {
            if(pathProxy) {
                check(info.getBoolean("pathOpens"),"Native path broker missing");external.serveOpens();
                publish(new File(root,"tmp/media-proxy-host-ready"),runId.getBytes("US-ASCII"));
                File done=new File(root,"tmp/media-proxy-app-done");long until=SystemClock.elapsedRealtime()+90000;
                while(!done.isFile()){
                    String logs=nativeLog(),marker="POCKET_MEDIA_SESSION32_RESULT ";int at=logs.indexOf(marker);
                    if(at>=0){int end=logs.indexOf('\n',at);JSONObject media=new JSONObject(logs.substring(at+marker.length(),end<0?logs.length():end));result.put("mediaSession",media);check(!media.optString("validation").equals("FAIL"),media.optString("error"));}
                    check(SystemClock.elapsedRealtime()<until,"Guest ordinary-path operations did not finish");Thread.sleep(200);
                }
                byte[] completion=Files.readAllBytes(done.toPath());result.put("appPathDoneBytes",completion.length);
                check(runId.equals(new String(completion,"US-ASCII")),"Wrong app path nonce: bytes="+completion.length);
            }else {
            JSONArray capabilities=info.getJSONArray("files");check(capabilities.length()==2,"Expected two proxy capabilities");FileDescriptor[] descriptors=new FileDescriptor[2];
            for(int i=0;i<2;i++) {
                JSONObject entry=capabilities.getJSONObject(i);int mode=entry.getInt("mode")==2?ParcelFileDescriptor.MODE_READ_WRITE:ParcelFileDescriptor.MODE_READ_ONLY;
                descriptors[i]=external.attach(entry.getInt("slot"),entry.getLong("cookie"),entry.getInt("flags"),mode).getFileDescriptor();
            }
            android.net.LocalSocket peer=null;long until=SystemClock.elapsedRealtime()+20000;
            while(peer==null) {
                android.net.LocalSocket attempt=new android.net.LocalSocket();
                try{attempt.connect(new android.net.LocalSocketAddress(new File(root,"dev/socket/pme32").getPath(),android.net.LocalSocketAddress.Namespace.FILESYSTEM));peer=attempt;}
                catch(IOException error){attempt.close();check(SystemClock.elapsedRealtime()<until,"Guest did not open descriptor endpoint");Thread.sleep(50);}
            }
            try(android.net.LocalSocket channel=peer) {
                String stat=new String(Files.readAllBytes(new File("/proc/"+info.getInt("pid")+"/stat").toPath()),"UTF-8");String[] fields=stat.substring(stat.lastIndexOf(')')+2).trim().split(" +");
                android.net.Credentials credentials=channel.getPeerCredentials();check(credentials.getUid()==android.os.Process.myUid() && credentials.getPid()==Integer.parseInt(fields[1]),"Proxy recipient is not the broker actor's parent app");
                channel.setSoTimeout(20000);channel.setFileDescriptorsForSend(descriptors);channel.getOutputStream().write('F');channel.setFileDescriptorsForSend(null);
                check(channel.getInputStream().read()=='D',"Guest proxy data operations failed");
            }
            }
            byte[] backing=Files.readAllBytes(mediaCanary.toPath());
            check(new String(backing,128,11,"UTF-8").equals("proxy-write") && new String(backing,256,16,"UTF-8").equals("shared-media-map"),"AppFuse writes/mmap did not reach MediaProvider backing file");
            external.close();JSONObject stats=external.report();check(stats.getInt("releasedFiles")== (pathProxy?3:2) && stats.getInt("remainingSlots")==0,"Real proxy handles were not released");result.put("externalFiles",stats).put("backingDataVerified",true);
            }finally{result.put("externalFiles",external.report());}
        }
        publish(new File(root,"tmp/media-proxy-done"),runId.getBytes("US-ASCII"));proxyFinished=true;
    }
    private void lazyMedia()throws Exception {
        if(lazyMediaProbe==null)return;
        File output=new File(root,"logs/media-activation.log");ProcessBuilder command=new ProcessBuilder(SyscallBackend.command(root,"/",Arrays.asList("/tmp/pocket-lazy-media-probe"))).directory(root);
        command.environment().clear();command.environment().putAll(SyscallBackend.environment(root,android.os.Process.myPid(),32));command.redirectErrorStream(true).redirectOutput(output);
        command.environment().put("LD_PRELOAD","/system/lib64/libpocket_binder_api32.so:/system/lib64/libpocket_hidl_api32.so");
        java.lang.Process probe=command.start();
        try {
            check(probe.waitFor(40,java.util.concurrent.TimeUnit.SECONDS),"Lazy media activation timed out");
            String log=new String(Files.readAllBytes(output.toPath()),"UTF-8");result.put("lazyMediaActivation",new JSONObject().put("exit",probe.exitValue()).put("log",log));
            check(probe.exitValue()==0 && log.contains("API32_LAZY_MEDIA_PASS"),"Lazy media activation failed: "+log);
        }finally{if(probe.isAlive())probe.destroyForcibly();}
    }
    private void clearPropertyAreas()throws Exception {
        check(DirectImage.leftovers(root).isEmpty(),"An existing guest owns the property areas");
        File[] files=new File(root,"dev/__properties__").listFiles();check(files!=null,"Missing property directory");
        for(File file:files){check(file.isFile() && !Files.isSymbolicLink(file.toPath()),"Unexpected volatile property entry");Files.delete(file.toPath());}
    }
    private void metrics()throws Exception {
        JSONArray checks=new JSONArray();result.put("coreMediaMetrics",checks);
        for(String mode:new String[]{"submit","verify"}) {
            File output=new File(root,"logs/media-metrics-"+mode+".log");
            ProcessBuilder command=new ProcessBuilder(SyscallBackend.command(root,"/",Arrays.asList("/tmp/pocket-metrics-probe",mode,runId))).directory(root);
            command.environment().clear();command.environment().putAll(SyscallBackend.environment(root,android.os.Process.myPid(),32));
            command.environment().put("LD_PRELOAD","/system/lib64/libpocket_binder_api32.so");command.redirectErrorStream(true).redirectOutput(output);
            java.lang.Process probe=command.start();
            try {
                check(probe.waitFor(30,java.util.concurrent.TimeUnit.SECONDS),"Metrics caller-policy check timed out");
                check(output.length()<65536,"Metrics caller-policy output exceeds limit");
                String log=new String(Files.readAllBytes(output.toPath()),"UTF-8");checks.put(new JSONObject().put("mode",mode).put("exit",probe.exitValue()).put("log",log));
                check(probe.exitValue()==0 && log.contains(mode.equals("submit")?"API32_CORE_METRICS_SUBMITTED":"API32_CORE_METRICS_VERIFIED"),"Stock media.metrics check failed: "+mode+" exit="+probe.exitValue()+" "+log);
            }finally{if(probe.isAlive())probe.destroyForcibly();}
        }
    }
    private void bootstrapFailures()throws Exception {
        if(!supervisedBootstrap)return;
        JSONArray checks=new JSONArray();
        for(int exit:new int[]{7,0}) {
            clearPropertyAreas();
            Files.deleteIfExists(new File(root,"tmp/pocket-runtime-ready").toPath());
            File script=new File(root,"tmp/bootstrap-negative-"+exit+".sh"),config=new File(root,"tmp/bootstrap-negative-"+exit+".rc"),output=new File(root,"logs/bootstrap-negative-"+exit+".log");
            for(File file:new File[]{script,config}){check(!file.exists(),"Bootstrap failure fixture already exists");created.add(file);}
            Files.write(script.toPath(),("builtin print -r -- '-n' '-e'\nexit "+exit+"\n").getBytes("US-ASCII"),StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE);
            String rc="on boot\n    start pocket-runtime-bootstrap\n\nservice pocket-runtime-bootstrap /system/bin/sh /tmp/"+script.getName()+"\n    disabled\n    oneshot\n    user 0\n    group 0\n";
            Files.write(config.toPath(),rc.getBytes("US-ASCII"),StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE);
            ProcessBuilder command=new ProcessBuilder(SyscallBackend.command(root,"/",Arrays.asList("/system/bin/pocket-init-api32","/tmp/"+config.getName()))).directory(root);
            command.environment().clear();command.environment().putAll(SyscallBackend.environment(root,android.os.Process.myPid(),32));command.environment().put("POCKET_API32_RUN_ID",runId);command.redirectErrorStream(true).redirectOutput(output);
            java.lang.Process owner=command.start();
            try {
                check(owner.waitFor(20,java.util.concurrent.TimeUnit.SECONDS),"Bootstrap failure owner did not exit");
                String log=new String(Files.readAllBytes(output.toPath()),"UTF-8");checks.put(new JSONObject().put("childExit",exit).put("ownerExit",owner.exitValue()).put("log",log));result.put("bootstrapFailureChecks",checks);
                check(log.contains("\n-n -e\n"),"Android shell builtin did not preserve option-like nonces");
                check(owner.exitValue()==67 && log.contains("API32_RUNTIME_BOOTSTRAP_FAILED") && log.contains("remaining=0 failed=1"),"Bootstrap failure was not supervised");
                check(!new File(root,"tmp/pocket-runtime-ready").exists() && DirectImage.leftovers(root).isEmpty(),"Failed bootstrap published readiness or left a child");
            }finally{if(owner.isAlive()){owner.destroy();if(!owner.waitFor(5,java.util.concurrent.TimeUnit.SECONDS))owner.destroyForcibly();}}
        }
        clearPropertyAreas();
    }
    @Override protected void probe() {
        try {
            result.put("runId",runId).put("sdk",32);stage("preparing-media-session");patch();
            long deadline=SystemClock.elapsedRealtime()+30000;
            while(!ui(()->runtime!=null)){check(SystemClock.elapsedRealtime()<deadline,"Manager service did not bind");Thread.sleep(100);}
            if(supervisedBootstrap){NativeLoader.prepare(root);stage("verifying-bootstrap-failures");bootstrapFailures();}
            ui(()->{runtime.startGuest();return null;});stage("booting");boot();result.put("homeColors",frame("media-session-home.png"));
            if(lazyMediaProbe!=null){stage("verifying-lazy-media");lazyMedia();}
            if(metricsProbe!=null){stage("verifying-core-media-metrics");metrics();}
            File apk=new File(getFilesDir(),"media-session.apk");try(InputStream in=getAssets().open("api32-media-session.apk");OutputStream out=new FileOutputStream(apk)){GuestImage.copy(in,out,1024*1024);}
            stage("starting-real-mediaprovider");
            if(cachedFixture(apk)) {
                result.put("cachedFixtureVerified",true);
                File output=new File(root,"logs/media-launch.log");ProcessBuilder command=new ProcessBuilder(SyscallBackend.command(root,"/",Arrays.asList("/system/bin/pocket-manager-api32","open","dev.kreijstal.pocketmediasession32"))).directory(root);
                command.environment().clear();command.environment().putAll(SyscallBackend.environment(root,android.os.Process.myPid(),32));command.redirectErrorStream(true).redirectOutput(output);
                java.lang.Process launch=command.start();try{check(launch.waitFor(45,java.util.concurrent.TimeUnit.SECONDS) && launch.exitValue()==0,"Cached fixture launch failed");}finally{if(launch.isAlive())launch.destroyForcibly();}
            }else ui(()->{runtime.installApps(Collections.singletonMap("base.apk",Uri.fromFile(apk)));return null;});
            deadline=SystemClock.elapsedRealtime()+180000;
            while(true) {
                if(proxy && !proxyFinished && new File(root,"tmp/media-proxy-ready").isFile())proxy();
                String logs=nativeLog();String marker="POCKET_MEDIA_SESSION32_RESULT ";int at=logs.indexOf(marker);
                if(at>=0){int end=logs.indexOf('\n',at);JSONObject media=new JSONObject(logs.substring(at+marker.length(),end<0?logs.length():end));
                    check(media.getString("runId").equals(runId),"Wrong MediaProvider result");result.put("mediaSession",media);check(media.getString("validation").equals("PASS"),media.optString("error"));break;}
                JSONObject owner=session(false);check(!owner.has("ownerExitCode") || owner.optBoolean("ownerAlive",false) || owner.optInt("ownerExitCode")==0,"Guest supervisor exited: "+owner.optInt("ownerExitCode"));
                check(SystemClock.elapsedRealtime()<deadline,"Real MediaProvider probe did not report");Thread.sleep(500);
            }
            if(systemServerBridge) {
                stage("verifying-embedded-input");
                ui(()->{runtime.key(android.view.KeyEvent.KEYCODE_WAKEUP);return null;});Thread.sleep(500);
                ui(()->{
                    runtime.key(android.view.KeyEvent.KEYCODE_9);
                    long now=SystemClock.uptimeMillis();
                    for(int action:new int[]{android.view.MotionEvent.ACTION_DOWN,android.view.MotionEvent.ACTION_UP}) {
                        android.view.MotionEvent event=android.view.MotionEvent.obtain(now,SystemClock.uptimeMillis(),action,runtime.width/2f,runtime.height/2f,0);
                        try{runtime.touch(event,1,1);}finally{event.recycle();}
                    }
                    return null;
                });
                deadline=SystemClock.elapsedRealtime()+15000;
                String uid=" uid="+result.getJSONObject("mediaSession").getInt("uid");
                for(;;) {
                    String logs=nativeLog();
                    if(logs.contains("POCKET_MEDIA_INPUT32_KEY run="+runId+uid) && logs.contains("POCKET_MEDIA_INPUT32_TOUCH run="+runId+uid))break;
                    check(SystemClock.elapsedRealtime()<deadline,"Input did not reach the ordinary app through system_server");Thread.sleep(100);
                }
                JSONObject input=session(false).getJSONObject("input");
                check(input.getLong("keyEventsAccepted")>=2 && input.getLong("touchEventsAccepted")>=2 && input.getLong("inputEventsRejected")==0,"Embedded input transport rejected an event");
                result.put("systemServerBridge",new JSONObject().put("keyDelivered",true).put("touchDelivered",true).put("input",input));
            }
            result.put("functionalPassed",true);
        }catch(Throwable error){try{result.put("functionalPassed",false).put("error",error.toString());}catch(Exception ignored){}}
        finally {
            try{stage("stopping");stop();result.put("stoppedSession",session(false));restore();}
            catch(Exception error){try{result.put("functionalPassed",false).put("cleanupError",error.toString());}catch(Exception ignored){}}
            try {
                result.put("console",console());JSONObject state=session(true);String logs=state.optString("nativeLog");result.put("nativeLog",logs);
                if(!result.has("mediaSession")) {
                    String marker="POCKET_MEDIA_SESSION32_RESULT ";int at=logs.indexOf(marker);
                    if(at>=0){int end=logs.indexOf('\n',at);result.put("mediaSession",new JSONObject(logs.substring(at+marker.length(),end<0?logs.length():end)));}
                }
                File operation=new File(root,"logs/manager-operation.log");if(operation.isFile() && operation.length()<65536)result.put("installOperation",new String(Files.readAllBytes(operation.toPath()),"UTF-8"));
                boolean clean=!logs.contains("ANR in ") && !logs.contains("FATAL EXCEPTION:") && !logs.contains("Fatal signal ") && logs.length()<2*1024*1024;
                boolean stopped=state.has("ownerExitCode") && state.getInt("ownerExitCode")==0 && !state.optBoolean("ownerAlive",true) && !state.has("cleanupFailure");
                result.put("startupClean",clean).put("supervisedStop",stopped).put("validation",result.optBoolean("functionalPassed") && clean && stopped?"PASS":"FAIL");
                stage("finished");finished=result.toString();Files.write(new File(getFilesDir(),"manager32-media-result.json").toPath(),finished.getBytes("UTF-8"));
            }catch(Exception error){try{result.put("validation","FAIL").put("reportError",error.toString());finished=result.toString();}catch(Exception ignored){}}
        }
    }
}
