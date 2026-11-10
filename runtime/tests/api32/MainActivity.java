// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.api32test;
import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
import java.io.*;
import java.nio.file.*;
import java.util.concurrent.TimeUnit;
import android.system.Os;
import android.system.OsConstants;
import android.system.ErrnoException;
import android.content.SharedPreferences;
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.security.MessageDigest;
import org.json.JSONObject;
import dev.kreijstal.aosppocket.runtime.HostBinder;
import dev.kreijstal.aosppocket.runtime.HostHardware;

/** Separate sandbox/package/data: the working Android 9 profile is untouched. */
public final class MainActivity extends Activity {
    private volatile dev.kreijstal.aosppocket.runtime.HostDisplay32 currentDisplay;
    private volatile NativeLog currentNativeLog;
    private volatile String currentRunId;
    private volatile boolean runtimeStarted;
    private volatile boolean reportReady;
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);TextView status=new TextView(this);status.setText("Android 12L runtime test…");
        final android.view.SurfaceView display;
        if(getIntent().getBooleanExtra("probe_display",false)) {
            getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            status.setMaxLines(3);android.widget.LinearLayout layout=new android.widget.LinearLayout(this);layout.setOrientation(1);
            layout.addView(status,new android.widget.LinearLayout.LayoutParams(-1,-2));
            display=new android.view.SurfaceView(this);layout.addView(display,new android.widget.LinearLayout.LayoutParams(-1,0,1));setContentView(layout);
        } else {display=null;setContentView(status);}
        final String runId=getIntent().getStringExtra("probe_run_id")==null?java.util.UUID.randomUUID().toString().replace("-",""):getIntent().getStringExtra("probe_run_id");
        currentRunId=runId;
        if(getIntent().getBooleanExtra("probe_diagnostics_only",false)) {
            reportReady=true;
            status.setText("Android 12L saved diagnostics");return;
        }
        new Thread(()->{
            JSONObject report=new JSONObject();
            HostBinder binder=null;
            HostHardware hardware=null;
            NativeLog nativeLog=null;
            dev.kreijstal.aosppocket.runtime.HostDns dns=null;
            dev.kreijstal.aosppocket.runtime.HostAudio audio=null;
            dev.kreijstal.aosppocket.runtime.HostDisplay32 presentation=null;
            File root=new File(getFilesDir(),"root");
            try {
                report.put("runId",runId);
                stopGuest(root);
                try(java.net.DatagramSocket socket=new java.net.DatagramSocket(null)){
                    report.put("hostDatagramSocket",true);
                }catch(Exception error){report.put("hostDatagramSocket",error.toString());}
                long preparation=android.os.SystemClock.elapsedRealtime();
                report.put("extractedFiles",extract(root));
                report.put("extractionMillis",android.os.SystemClock.elapsedRealtime()-preparation);
                preparation=android.os.SystemClock.elapsedRealtime();
                report.put("imageVerifiedFiles",verifyImage(root));
                report.put("imageVerificationMillis",android.os.SystemClock.elapsedRealtime()-preparation);
                for(String name:new String[]{".pocket-owner","data/local/tmp","data/dalvik-cache/arm64","data/resource-cache","dev/__properties__","dev/socket","tmp","mnt/asec","mnt/expand","linkerconfig"})
                    if(!new File(root,name).isDirectory() && !new File(root,name).mkdirs())throw new IOException("Cannot create runtime directory");
                Path etc=new File(root,"etc").toPath();
                if(!Files.exists(etc,LinkOption.NOFOLLOW_LINKS))Files.createSymbolicLink(etc,Paths.get("system/etc"));
                // Regenerate dynamic namespace files on every owned fixture boot.
                File config=new File(root,"linkerconfig");
                clearConfig(config);
                Files.copy(new File(root,"bootstrap-linkerconfig").toPath(),new File(config,"ld.config.txt").toPath(),StandardCopyOption.REPLACE_EXISTING);
                // Property areas belong to one boot. Old read-only values must
                // not survive a restart of this isolated fixture.
                File[] oldProperties=new File(root,"dev/__properties__").listFiles();
                if(oldProperties==null)throw new IOException("Cannot access fixture properties");
                for(File entry:oldProperties)
                    if(!entry.delete())throw new IOException("Cannot clear fixture property area");
                if(new String(Files.readAllBytes(new File(root,"probe-binder-enabled").toPath()),"UTF-8").equals("1"))
                    binder=new HostBinder(this,root);
                nativeLog=new NativeLog(root);
                currentNativeLog=nativeLog;
                File dnsFlag=new File(root,"probe-dns-enabled");
                if(dnsFlag.isFile() && new String(Files.readAllBytes(dnsFlag.toPath()),"UTF-8").equals("1"))
                    dns=new dev.kreijstal.aosppocket.runtime.HostDns(this,root);
                File audioFlag=new File(root,"probe-audio-enabled");
                if(audioFlag.isFile() && new String(Files.readAllBytes(audioFlag.toPath()),"UTF-8").equals("1"))
                    audio=new dev.kreijstal.aosppocket.runtime.HostAudio(root);
                if(new File(root,"probe-hardware-enabled").isFile() &&
                    new String(Files.readAllBytes(new File(root,"probe-hardware-enabled").toPath()),"UTF-8").equals("1"))
                    hardware=new HostHardware(this,root);
                File log=new File(getFilesDir(),"native.log");
                ProcessBuilder builder=new ProcessBuilder(new File(root,"system/bin/sh").getPath(),"/probe.sh").directory(root);
                builder.environment().clear();
                for(String[] env:new String[][]{{"POCKET_LOADER_BOOT","1"},{"ANDROID_ROOT","/system"},{"ANDROID_DATA","/data"},
                        {"ANDROID_RUNTIME_ROOT","/apex/com.android.runtime"},{"ANDROID_TZDATA_ROOT","/apex/com.android.tzdata"},{"EXTERNAL_STORAGE","/sdcard"},
                        {"ASEC_MOUNTPOINT","/mnt/asec"},
                        {"PATH","/system/bin:/apex/com.android.runtime/bin"}})builder.environment().put(env[0],env[1]);
                String[] props=new String(Files.readAllBytes(new File(root,"probe-environment").toPath()),"UTF-8").split("\n");
                for(String entry:props){int split=entry.indexOf('=');if(split>0)builder.environment().put(entry.substring(0,split),entry.substring(split+1));}
                if(getIntent().getBooleanExtra("probe_ui",false))builder.environment().put("POCKET_API32_REQUIRE_UI","1");
                builder.environment().put("POCKET_API32_RUN_ID",runId);
                builder.environment().put("POCKET_API32_HOST_PID",String.valueOf(android.os.Process.myPid()));
                String hostStat=new String(Files.readAllBytes(Paths.get("/proc/self/stat")),"UTF-8");
                builder.environment().put("POCKET_API32_HOST_START",hostStat.substring(hostStat.lastIndexOf(')')+2).trim().split(" +")[19]);
                builder.redirectInput(new File("/dev/null")).redirectErrorStream(true).redirectOutput(log);
                Process child=builder.start();
                runtimeStarted=true;
                if(display!=null) {
                    presentation=new dev.kreijstal.aosppocket.runtime.HostDisplay32(root,log,display,child,runId);
                    currentDisplay=presentation;
                }
                boolean finished=child.waitFor(builder.environment().containsKey("POCKET_API32_APP_PROBE")?600:
                    builder.environment().containsKey("POCKET_API32_UI_PROBE")?300:120,TimeUnit.SECONDS);
                if(!finished){
                    child.destroy();
                    if(!child.waitFor(5,TimeUnit.SECONDS)){child.destroyForcibly();child.waitFor(5,TimeUnit.SECONDS);}
                }
                String output=new String(Files.readAllBytes(log.toPath()),"UTF-8");
                File runtimeFlag=new File(root,"probe-runtime-boot-enabled");
                boolean runtimeBoot=runtimeFlag.isFile() && new String(Files.readAllBytes(runtimeFlag.toPath()),"UTF-8").equals("1");
                boolean successful=finished && child.exitValue()==0 && output.contains(runtimeBoot?"POCKET_RUNTIME_VALIDATION_PASS":"API32_NATIVE_RUNTIME_PASS");
                File initLog=new File(root,"tmp/properties.log");
                if(!output.contains("API32_PROPERTIES_READY") && initLog.isFile())
                    output+="\n"+new String(Files.readAllBytes(initLog.toPath()),"UTF-8");
                report.put("hostUid",android.os.Process.myUid()).put("exitCode",child.isAlive()?-1:child.exitValue()).put("log",output)
                    .put("passed",successful)
                    .put("runtimeBootRequested",runtimeBoot)
                    .put("androidAppSandboxVerified",finished && child.exitValue()==0)
                    .put("fullGuestBootVerified",false)
                    .put("scope","Isolated Android 12L native/ART bring-up; framework and Manager integration not yet verified");
                if(!finished)report.put("error","Android 12L probe timed out");
            }catch(Throwable error){try{report.put("passed",false).put("error",error.toString());}catch(Exception ignored){}}
            finally{
                if(presentation!=null)try{presentation.close();report.put("hostDisplay",presentation.report());}catch(Exception error){try{report.put("displayError",error.toString());}catch(Exception ignored){}}
                try{stopGuest(root);}catch(Exception error){try{report.put("passed",false).put("cleanupError",error.toString());}catch(Exception ignored){}}
                if(nativeLog!=null){
                    try{nativeLog.close();report.put("nativeLog",nativeLog.text());}
                    catch(Exception error){try{report.put("passed",false).put("logCollectorError",error.toString());}catch(Exception ignored){}}
                }
                if(binder!=null)binder.close();
                if(hardware!=null)hardware.close();
                if(dns!=null)dns.close();
                if(audio!=null){audio.close();try{report.put("audioHost",audio.report());}catch(Exception ignored){}}
            }
            try{
                Path temporary=new File(getFilesDir(),"report.json.new").toPath();
                Files.write(temporary,report.toString(2).getBytes("UTF-8"));
                Files.move(temporary,new File(getFilesDir(),"report.json").toPath(),StandardCopyOption.REPLACE_EXISTING,StandardCopyOption.ATOMIC_MOVE);
                reportReady=true;
            }catch(Exception ignored){}
            runOnUiThread(()->status.setText(report.toString()));
        },"PocketApi32Probe").start();
    }
    @Override public void dump(String prefix,FileDescriptor fd,PrintWriter writer,String[] args) {
        if(args!=null && java.util.Arrays.asList(args).contains("pocket-api32-display")) {
            try {
                JSONObject result=new JSONObject().put("runId",currentRunId).put("runtimeStarted",runtimeStarted);
                if(currentDisplay!=null)result.put("display",currentDisplay.report());
                writer.println("POCKET_API32_DISPLAY="+android.util.Base64.encodeToString(result.toString().getBytes("UTF-8"),android.util.Base64.NO_WRAP));
            }catch(Exception error){writer.println("POCKET_API32_DISPLAY_ERROR="+error);}
            return;
        }
        if(args!=null && java.util.Arrays.asList(args).contains("pocket-api32-anr")) {
            // Only this isolated app's saved ANR files. The host DUMP permission
            // mediates access; there is no arbitrary-path or guest command API.
            try {
                boolean detail=java.util.Arrays.asList(args).contains("pocket-api32-anr-detail");
                JSONObject diagnostics=new JSONObject();org.json.JSONArray traces=new org.json.JSONArray();
                File report=new File(getFilesDir(),"report.json");
                if(report.isFile() && report.length()<4*1024*1024)
                    diagnostics.put("runId",new JSONObject(new String(Files.readAllBytes(report.toPath()),"UTF-8")).optString("runId"));
                for(String data:new String[]{"root/data/anr","root/system-data/anr","root/tmp/art-traces",
                        "root/data/tombstones","root/system-data/tombstones"}) {
                    File directory=new File(getFilesDir(),data);
                    if(Files.isSymbolicLink(directory.toPath()))continue;
                    File[] files=directory.listFiles();if(files==null)continue;
                    java.util.Arrays.sort(files,(a,b)->Long.compare(b.lastModified(),a.lastModified()));
                    int count=0;
                    boolean art=data.equals("root/tmp/art-traces"),tombstones=data.endsWith("/tombstones");
                    java.util.HashMap<String,Integer> perProcess=new java.util.HashMap<>();
                    for(File file:files) {
                        if(count>=(detail?1:8))break;
                        String name=file.getName();
                        boolean stockTrace=data.endsWith("/anr") && name.matches("trace_\\d+");
                        boolean accepted=tombstones?name.matches("tombstone_\\d+"):name.startsWith("anr_") || stockTrace;
                        if(!accepted || !file.isFile() || Files.isSymbolicLink(file.toPath()) ||
                           !file.getCanonicalPath().startsWith(directory.getCanonicalPath()+"/"))continue;
                        if(art) {
                            String[] parts=file.getName().split("_");
                            if(parts.length!=4 || !parts[1].matches("\\d+"))continue;
                            int samples=perProcess.containsKey(parts[1])?perProcess.get(parts[1]):0;
                            if(samples>=2)continue;
                            perProcess.put(parts[1],samples+1);
                        }
                        try(RandomAccessFile input=new RandomAccessFile(file,"r")) {
                            byte[] bytes=new byte[(int)Math.min(input.length(),(detail || art)?512*1024:(stockTrace || tombstones)?128*1024:64*1024)];input.readFully(bytes);
                            traces.put(new JSONObject().put("path",data+"/"+file.getName()).put("bytes",input.length())
                                .put("modified",file.lastModified()).put("truncated",input.length()>bytes.length)
                                .put("content",new String(bytes,"UTF-8")));
                        }
                        count++;
                    }
                }
                diagnostics.put("traces",traces);
                writer.println("POCKET_API32_ANR="+android.util.Base64.encodeToString(diagnostics.toString().getBytes("UTF-8"),android.util.Base64.NO_WRAP));
            } catch(Exception error){writer.println("POCKET_API32_ANR_ERROR="+error);}
            return;
        }
        if(args!=null && java.util.Arrays.asList(args).contains("pocket-api32-progress")) {
            // A separate key cannot be mistaken for the terminal report. As
            // with report dumping, the host system mediates DUMP permission.
            try {
                JSONObject progress=new JSONObject().put("runId",currentRunId).put("runtimeStarted",runtimeStarted);
                if(runtimeStarted) {
                  for(String[] entry:new String[][]{{"log","native.log"},{"initLog","root/tmp/properties.log"}}) {
                    File path=new File(getFilesDir(),entry[1]);
                    if(!path.isFile())continue;
                    try(RandomAccessFile input=new RandomAccessFile(path,"r")) {
                        int length=(int)Math.min(input.length(),256*1024);
                        byte[] bytes=new byte[length];input.seek(input.length()-length);input.readFully(bytes);
                        progress.put(entry[0],new String(bytes,"UTF-8"));
                    }
                  }
                  NativeLog log=currentNativeLog;
                  if(log!=null) {
                    String value=log.text();progress.put("nativeLog",value.substring(Math.max(0,value.length()-512*1024)));
                  }
                }
                writer.println("POCKET_API32_PROGRESS="+android.util.Base64.encodeToString(progress.toString().getBytes("UTF-8"),android.util.Base64.NO_WRAP));
            }catch(Exception error){writer.println("POCKET_API32_PROGRESS_ERROR="+error);}
            return;
        }
        if(args==null || !java.util.Arrays.asList(args).contains("pocket-api32-report")){
            super.dump(prefix,fd,writer,args);return;
        }
        // Activity dumping is mediated by the host system's DUMP permission.
        // Return only this fixture's report, without requiring root or run-as.
        // Do not read/encode the previous multi-megabyte report on every poll
        // while the new guest starts. The driver must only accept terminal
        // data; this distinct pending key cannot look like a completed run.
        if(!reportReady) {
            writer.println("POCKET_API32_REPORT_PENDING run_id="+currentRunId);
            return;
        }
        File report=new File(getFilesDir(),"report.json");
        if(!report.isFile())return;
        try{
            byte[] bytes=Files.readAllBytes(report.toPath());
            if(bytes.length>4*1024*1024)throw new IOException("Fixture report too large");
            writer.println("POCKET_API32_REPORT="+android.util.Base64.encodeToString(bytes,android.util.Base64.NO_WRAP));
        }catch(Exception error){writer.println("POCKET_API32_REPORT_ERROR="+error);}
    }
    /** Kill only executables inside this fixture's private root, as its app UID. */
    private void stopGuest(File root)throws Exception {
        String prefix=root.getCanonicalPath()+"/";
        for(int attempt=0;attempt<40;attempt++){
            File[] entries=new File("/proc").listFiles();
            if(entries==null)throw new IOException("Cannot inspect fixture processes");
            boolean found=false;
            for(File entry:entries){
                if(!entry.getName().matches("\\d+"))continue;
                try{
                    if(Os.stat(entry.getPath()).st_uid!=android.os.Process.myUid())continue;
                    if(!Os.readlink(new File(entry,"exe").getPath()).startsWith(prefix))continue;
                    found=true;
                    Os.kill(Integer.parseInt(entry.getName()),attempt<20?OsConstants.SIGTERM:OsConstants.SIGKILL);
                }catch(ErrnoException exited){/* Exited or inaccessible. */}
            }
            if(!found)return;
            Thread.sleep(100);
        }
        throw new IOException("Fixture guest processes did not exit");
    }
    private void clearConfig(File directory)throws Exception {
        if(Files.isSymbolicLink(directory.toPath()))throw new IOException("Unexpected linkerconfig symlink");
        File[] children=directory.listFiles();
        if(children==null)throw new IOException("Missing linkerconfig directory");
        for(File child:children){
            if(Files.isSymbolicLink(child.toPath()))throw new IOException("Unexpected linkerconfig entry symlink");
            if(child.isDirectory())clearConfig(child);
            if(!child.delete())throw new IOException("Cannot reset owned linkerconfig");
        }
    }
    private int extract(File root)throws Exception {
        if(Files.isSymbolicLink(root.toPath()))throw new IOException("Unexpected fixture root symlink");
        if(!root.isDirectory() && !root.mkdirs())throw new IOException("Cannot create fixture root");
        SharedPreferences cache=getSharedPreferences("fixture-assets",MODE_PRIVATE);
        SharedPreferences.Editor stamps=cache.edit();int copied=0;
        String prefix="assets/root/",base=root.getCanonicalPath()+"/";
        // The source is the installed, signed APK. Cache its CRC and size just
        // like Manager's bundled-image extraction, avoiding hundreds of MB of
        // repeated writes when only a service adapter changed.
        try(ZipFile apk=new ZipFile(getApplicationInfo().sourceDir)){
            Enumeration<? extends ZipEntry> entries=apk.entries();
            while(entries.hasMoreElements()){
                ZipEntry entry=entries.nextElement();
                if(!entry.getName().startsWith(prefix) || entry.isDirectory())continue;
                String name=entry.getName().substring(prefix.length());
                for(String part:name.split("/",-1))
                    if(part.isEmpty() || part.equals(".") || part.equals("..") || part.indexOf('\\')>=0)
                        throw new IOException("Invalid fixture asset path");
                File target=new File(root,name);
                if(!target.getCanonicalPath().startsWith(base))throw new IOException("Fixture asset escapes root");
                for(File parent=target;!parent.equals(root);parent=parent.getParentFile())
                    if(Files.isSymbolicLink(parent.toPath()))throw new IOException("Unexpected fixture symlink");
                String stamp=entry.getCrc()+":"+entry.getSize();
                if(stamp.equals(cache.getString(name,"")) && target.isFile() && target.length()==entry.getSize())continue;
                if(!target.getParentFile().isDirectory() && !target.getParentFile().mkdirs())throw new IOException("Cannot create fixture directory");
                File temporary=new File(target.getPath()+".new");
                if(Files.isSymbolicLink(temporary.toPath()))throw new IOException("Unexpected fixture temporary symlink");
                long written=0;
                try(InputStream in=apk.getInputStream(entry);OutputStream out=new FileOutputStream(temporary)){
                    byte[] buffer=new byte[65536];int count;
                    while((count=in.read(buffer))!=-1){out.write(buffer,0,count);written+=count;}
                }
                if(written!=entry.getSize())throw new IOException("Incomplete fixture asset");
                Os.chmod(temporary.getPath(),0755);
                Files.move(temporary.toPath(),target.toPath(),StandardCopyOption.REPLACE_EXISTING,StandardCopyOption.ATOMIC_MOVE);
                stamps.putString(name,stamp);copied++;
            }
        }
        if(!stamps.commit())throw new IOException("Cannot save fixture asset metadata");
        return copied;
    }
    private File imageFile(File root,String name)throws Exception {
        if(name.indexOf('\0')>=0)throw new IOException("Invalid fixture image path");
        for(String part:name.split("/",-1))
            if(part.isEmpty() || part.equals(".") || part.equals("..") || part.indexOf('\\')>=0)
                throw new IOException("Invalid fixture image path");
        File target=new File(root,name);
        if(!target.getCanonicalPath().startsWith(root.getCanonicalPath()+"/"))throw new IOException("Fixture image escapes root");
        for(File parent=target;!parent.equals(root);parent=parent.getParentFile())
            if(Files.isSymbolicLink(parent.toPath()))throw new IOException("Fixture image contains a symlink");
        return target;
    }
    private int verifyImage(File root)throws Exception {
        JSONObject manifest;
        try(InputStream input=getAssets().open("fixture-image.json");ByteArrayOutputStream output=new ByteArrayOutputStream()){
            byte[] buffer=new byte[32768];int count;
            while((count=input.read(buffer))!=-1){if(output.size()+count>8*1024*1024)throw new IOException("Fixture manifest too large");output.write(buffer,0,count);}
            manifest=new JSONObject(new String(output.toByteArray(),"UTF-8"));
        }
        if(manifest.getInt("schema")!=1 || manifest.getInt("sdk")!=32)throw new IOException("Unsupported fixture image");
        JSONObject files=manifest.getJSONObject("files");
        if(files.length()==0 || files.length()>20000)throw new IOException("Invalid fixture file count");
        java.util.Iterator<String> names=files.keys();long total=0;
        while(names.hasNext()){
            String name=names.next();JSONObject spec=files.getJSONObject(name);long size=spec.getLong("bytes");
            String expected=spec.getString("sha256");
            if(size<0 || size>2L*1024*1024*1024-total || !expected.matches("[0-9a-f]{64}"))throw new IOException("Invalid fixture file metadata");
            total+=size;File file=imageFile(root,name);
            if(!file.isFile() || file.length()!=size)throw new IOException("Missing or incomplete fixture file: "+name);
            MessageDigest hash=MessageDigest.getInstance("SHA-256");long read=0;
            try(InputStream input=new FileInputStream(file)){
                byte[] buffer=new byte[65536];int count;
                while((count=input.read(buffer))!=-1){read+=count;if(read>size)throw new IOException("Fixture file changed: "+name);hash.update(buffer,0,count);}
            }
            StringBuilder actual=new StringBuilder();
            for(byte value:hash.digest())actual.append("0123456789abcdef".charAt((value&255)>>>4)).append("0123456789abcdef".charAt(value&15));
            if(read!=size || !expected.equals(actual.toString()))throw new IOException("Fixture image checksum differs: "+name);
        }
        return files.length();
    }
}
