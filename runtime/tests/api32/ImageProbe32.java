// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Base64;
import android.widget.TextView;
import dev.kreijstal.aosppocket.bridge.DisplayGeometry;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

/** Real download/preparation under a non-debuggable host app UID; never boots another profile. */
public final class ImageProbe32 extends Activity {
    private volatile String state="{}";
    private volatile boolean offline;
    private String runId;
    private TextView label;
    private final JSONObject result=new JSONObject();
    private void stage(String name)throws Exception {
        result.put("stage",name);state=result.toString();
        runOnUiThread(()->label.setText("Android 12L image: "+name));
    }
    private static void check(boolean okay,String message)throws IOException {if(!okay)throw new IOException(message);}
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);Native.configureHost();
        runId=getIntent().getStringExtra("probe_run_id");
        if(runId==null || !runId.matches("[a-f0-9]{32}")){finish();return;}
        label=new TextView(this);setContentView(label);
        new Thread(this::probe,"PocketImage32").start();
    }
    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if(runId.equals(intent.getStringExtra("probe_run_id")) && intent.getBooleanExtra("continue_offline",false))offline=true;
    }
    @Override public void dump(String prefix,FileDescriptor fd,PrintWriter writer,String[] args) {
        writer.println("POCKET_IMAGE32="+Base64.encodeToString(state.getBytes(java.nio.charset.StandardCharsets.UTF_8),Base64.NO_WRAP));
    }
    private String command(File root,String... arguments)throws Exception {
        Process child=DirectImage.admin(root,"/",Arrays.asList(arguments));
        if(!child.waitFor(15,TimeUnit.SECONDS)){child.destroyForcibly();throw new IOException("Guest command timed out");}
        byte[] stdout=new byte[4096],stderr=new byte[4096];
        int n=child.getInputStream().read(stdout),e=child.getErrorStream().read(stderr);
        check(child.exitValue()==0,"Guest command exited "+child.exitValue()+": "+new String(stderr,0,Math.max(0,e),"UTF-8"));
        return new String(stdout,0,Math.max(0,n),"UTF-8").trim();
    }
    private void probe() {
        File root=DirectImage.root(this,32);Process properties=null;
        try {
            result.put("runId",runId).put("hostUid",android.os.Process.myUid()).put("sdk",32);
            for(int sdk:new int[]{28,29}) {
                Path sentinel=new File(DirectImage.root(this,sdk),"system-data/api32-image-preservation").toPath();
                Files.createDirectories(sentinel.getParent());Files.write(sentinel,("preserve-api"+sdk).getBytes("UTF-8"));
            }
            GuestImage.Progress progress=new GuestImage.Progress(){
                public void phase(String phase){try{stage(phase);}catch(Exception error){throw new IllegalStateException(error);}}
                public void update(long done,long total){}
            };
            stage("preparing");
            Path oldData=new File(getFilesDir(),"api32-native-rootfs/system-data/image32-preservation").toPath();
            byte[] previous=Files.isRegularFile(oldData)?Files.readAllBytes(oldData):null;
            DirectImage.prepare(this,new DisplayGeometry(480,800,160),60,32,progress);
            if(previous!=null) {
                check(Arrays.equals(previous,Files.readAllBytes(new File(root,"system-data/image32-preservation").toPath())),"12L path migration changed guest data");
                check(!new File(getFilesDir(),"api32-native-rootfs").exists(),"Previous image root remains after migration");
                result.put("pathMigrationDataPreserved",true);
            }
            check(GuestProfile.detect(root).sdk==32,"Wrong image SDK");
            check(!Files.isSymbolicLink(new File(root,"product").toPath()),"12L product partition became a legacy alias");
            check(Files.isRegularFile(new File(root,"system/bin/getprop").toPath(),LinkOption.NOFOLLOW_LINKS),"Missing native getprop");
            check(command(root,"/system/bin/toybox","echo","API32_RELOCATED_EXEC_PASS").equals("API32_RELOCATED_EXEC_PASS"),"Relocated ELF did not run");
            Path own=new File(root,"system-data/image32-preservation").toPath();Files.write(own,runId.getBytes("UTF-8"));
            result.put("imageId",new String(Files.readAllBytes(new File(root,".pocket-image-id").toPath()),"UTF-8").trim());
            stage("awaiting-offline");long until=SystemClock.elapsedRealtime()+120000;
            while(!offline){check(SystemClock.elapsedRealtime()<until,"Runner did not stop image server");Thread.sleep(100);}
            DirectImage.prepare(this,new DisplayGeometry(480,800,160),60,32,progress);
            check(new String(Files.readAllBytes(own),"UTF-8").equals(runId),"12L data was replaced");
            for(int sdk:new int[]{28,29})check(new String(Files.readAllBytes(new File(DirectImage.root(this,sdk),"system-data/api32-image-preservation").toPath()),"UTF-8").equals("preserve-api"+sdk),"Older profile data was replaced");
            result.put("offlinePreparation",true).put("dataPreserved",true).put("relocatedElfExecuted",true);
            File output=new File(root,"logs/image32-properties.log");
            ProcessBuilder builder=new ProcessBuilder(SyscallBackend.command(root,"/",Collections.singletonList("/system/bin/pocket-property-service-api32"))).directory(root);
            builder.environment().clear();builder.environment().putAll(SyscallBackend.environment(root,android.os.Process.myPid(),32));
            builder.redirectErrorStream(true).redirectOutput(output);properties=builder.start();
            until=SystemClock.elapsedRealtime()+15000;
            while(!output.isFile() || !new String(Files.readAllBytes(output.toPath()),"UTF-8").contains("API32_PROPERTIES_READY")) {
                check(properties.isAlive() && SystemClock.elapsedRealtime()<until,"Guest properties did not start");Thread.sleep(100);
            }
            check(command(root,"/system/bin/getprop","ro.build.version.sdk").equals("32"),"Native getprop did not read the SDK32 property area");
            result.put("offlinePreparation",true).put("dataPreserved",true).put("relocatedElfExecuted",true).put("nativeGetprop",true).put("validation","PASS");
        }catch(Throwable error){try{
            result.put("validation","FAIL").put("error",error.toString());
            File log=new File(root,"logs/image32-properties.log");
            if(log.isFile()) {
                try(InputStream input=new FileInputStream(log)) {
                    byte[] bytes=new byte[8192];int count=input.read(bytes);
                    result.put("propertyLog",new String(bytes,0,Math.max(0,count),"UTF-8"));
                }
            }
            if(properties!=null && !properties.isAlive())result.put("propertyExitCode",properties.exitValue());
        }catch(Exception ignored){}}
        finally {
            try{stage("cleaning");}catch(Exception ignored){}
            if(properties!=null){properties.destroy();try{if(!properties.waitFor(5,TimeUnit.SECONDS))properties.destroyForcibly();}catch(InterruptedException interrupted){properties.destroyForcibly();}}
            try{check(DirectImage.leftovers(root).isEmpty(),"Guest command processes remain");}
            catch(Exception error){try{result.put("validation","FAIL").put("cleanupError",error.toString());}catch(Exception ignored){}}
            try{stage("PASS".equals(result.optString("validation"))?"finished":"failed");}catch(Exception ignored){}
            try{Files.write(new File(getFilesDir(),"image32-result.json").toPath(),state.getBytes("UTF-8"));}catch(IOException ignored){}
        }
    }
}
