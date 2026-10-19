// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.system.Os;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.LinearLayout;
import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;
import org.json.JSONObject;

/** Run the existing real platform regression with a direct process launcher. */
public final class BootProbeActivity extends Api28ProbeActivity {
    private boolean systemServerMode;
    private List<String> overlayFiles=Collections.emptyList();
    private BootBattery batteryRelay;
    private volatile boolean destroyed;
    @Override protected File extract() throws Exception {
        File root = new File(getFilesDir(), "api28-rootfs");
        File data = new File(root, "data");
        if (Files.isSymbolicLink(data.toPath())) Files.delete(data.toPath());
        File zygote = new File(root, "zygote-data");
        if (Files.isSymbolicLink(zygote.toPath())) Files.delete(zygote.toPath());
        BootOverlay.reset(this,root);
        root = super.extract();
        getExternalFilesDir(null);
        if (getIntent().getBooleanExtra("native_overlay",false)) overlayFiles=BootOverlay.apply(this,root);
        new File(root, "logs").mkdirs();
        batteryRelay=new BootBattery(this,root);
        for (File file : new File(root, "system/bin").listFiles()) patchInterpreter(file, new File(root, "system/bin/linker64").getPath());
        File config = new File(root, "system/etc/ld.config.txt");
        Files.write(config.toPath(), ("dir.pocket = " + root.getPath() + "\n[pocket]\n"
            + "namespace.default.isolated = false\nnamespace.default.search.paths = " + root + "/system/lib64:"+root+"/vendor/lib64\n"
            + "namespace.default.permitted.paths = " + root + "\n").getBytes("UTF-8"));
        return root;
    }
    @Override protected boolean extendedStage(File root, JSONObject result) throws Exception {
        result.put("native_overlay_files",new org.json.JSONArray(overlayFiles));
        if (!"system-server".equals(getIntent().getStringExtra("stage"))) return false;
        message("Android 9: starting system_server on the direct backend…");
        systemServerMode=true;
        File systemData=new File(root,"system-data");
        if (Files.isSymbolicLink(systemData.toPath())) throw new IOException("Invalid system data path");
        systemData.mkdirs();
        java.lang.Process profile=startProgram(root,"pocket-boot-probe",new String[]{},"0:0","boot-profile");
        if (!profile.waitFor(15,java.util.concurrent.TimeUnit.SECONDS) || profile.exitValue()!=0)
            throw new IOException("Direct profile test failed: "+new String(Files.readAllBytes(programLog("boot-profile").toPath()),"UTF-8"));
        result.put("direct_profile_passed",true).put("direct_profile_log",new String(Files.readAllBytes(programLog("boot-profile").toPath()),"UTF-8"));
        java.lang.Process supervisor=startProgram(root,"pocket-init-api28",new String[]{"/system/etc/init/init.system-server.rc"},"0:0","zygote-init");
        try {
            boolean ready=false;
            for (int i=0;i<100 && supervisor.isAlive();++i) {
                if (programLog("zygote-init").isFile() && new String(Files.readAllBytes(programLog("zygote-init").toPath()),"UTF-8").contains("API28_INIT_READY")) { ready=true;break; }
                Thread.sleep(100);
            }
            if (!ready) throw new IOException("Init did not publish its private property service");
            java.lang.Process guard=startProgram(root,"pocket-boot-services",new String[]{"identity-server"},"0:0","identity-server");
            try {
                for (int i=0;i<100 && guard.isAlive();++i) {
                    if (new String(Files.readAllBytes(programLog("identity-server").toPath()),"UTF-8").contains("DIRECT_EARLY_IDENTITY_READY")) break;
                    Thread.sleep(100);
                }
                for (String uid:new String[]{"1000","39999"}) {
                    String label="identity-client-"+uid;
                    java.lang.Process peer=startProgram(root,"pocket-boot-services",new String[]{"identity-client"},uid+":"+uid,label);
                    if (!peer.waitFor(15,java.util.concurrent.TimeUnit.SECONDS) || peer.exitValue()!=0) {
                        peer.destroyForcibly();throw new IOException("Early Binder identity failed: "+uid);
                    }
                    result.put(label,new String(Files.readAllBytes(programLog(label).toPath()),"UTF-8"));
                }
                result.put("early_binder_identity_passed",true);
            } finally { guard.destroy();if (!guard.waitFor(3,java.util.concurrent.TimeUnit.SECONDS)) guard.destroyForcibly(); }
            java.lang.Process death=startProgram(root,"pocket-boot-services",new String[]{"identity-gone"},"39999:39999","registry-death");
            if (!death.waitFor(10,java.util.concurrent.TimeUnit.SECONDS) || death.exitValue()!=0) {
                death.destroyForcibly();throw new IOException("Registry retained a dead service");
            }
            result.put("registry_death_passed",true).put("registry_death_log",new String(Files.readAllBytes(programLog("registry-death").toPath()),"UTF-8"));
            java.lang.Process egl=startProgram(root,"pocket-boot-egl",new String[]{},"1000:1000","boot-egl");
            boolean eglPassed=egl.waitFor(20,java.util.concurrent.TimeUnit.SECONDS) && egl.exitValue()==0;
            if (egl.isAlive()) egl.destroyForcibly();
            result.put("egl_passed",eglPassed).put("egl_log",new String(Files.readAllBytes(programLog("boot-egl").toPath()),"UTF-8"));
            java.util.Set<Integer> generations=new java.util.LinkedHashSet<>();
            for (int i=0;i<180;++i) {
                Thread.sleep(1000);
                String current=new String(Files.readAllBytes(programLog("zygote-init").toPath()),"UTF-8");
                java.util.regex.Matcher pid=java.util.regex.Pattern.compile("DIRECT_ZYGOTE_FORK pid=([1-9][0-9]*) system_server=1").matcher(current);
                int latest=0;while(pid.find()) {latest=Integer.parseInt(pid.group(1));generations.add(latest);}
                if (latest>0 && result.optInt("system_server_pid",0)!=latest) {
                    try {
                        result.put("system_server_pid",latest)
                            .put("system_server_status",new String(Files.readAllBytes(Paths.get("/proc/"+latest+"/status")),"UTF-8"))
                            .put("system_server_selinux",new String(Files.readAllBytes(Paths.get("/proc/"+latest+"/attr/current")),"UTF-8"));
                    } catch(IOException missing) { /* Report process death through startup log. */ }
                }
                if(current.contains("DIRECT_FRAMEWORK_SMOKE_PASS") || current.contains("DIRECT_FRAMEWORK_SMOKE_FAIL") ||
                   current.contains("FATAL EXCEPTION IN SYSTEM PROCESS") || current.contains("DIRECT_CRASH ")) break;
            }
            result.put("system_server_generations",new org.json.JSONArray(generations));
            java.lang.Process bootStatus=startProgram(root,"pocket-boot-services",new String[]{"boot-status"},"1000:1000","boot-status");
            boolean readBoot=bootStatus.waitFor(5,java.util.concurrent.TimeUnit.SECONDS) && bootStatus.exitValue()==0;
            if(bootStatus.isAlive())bootStatus.destroyForcibly();
            String bootLog=new String(Files.readAllBytes(programLog("boot-status").toPath()),"UTF-8");
            result.put("system_server_boot_completed",readBoot && bootLog.contains("DIRECT_BOOT_STATUS boot_completed=1\n"))
                .put("boot_status_log",bootLog);
            java.lang.Process services=startProgram(root,"pocket-boot-services",new String[]{},"39999:39999","boot-services");
            boolean servicesPassed=services.waitFor(15,java.util.concurrent.TimeUnit.SECONDS) && services.exitValue()==0;
            if (services.isAlive()) services.destroyForcibly();
            result.put("system_server_services_passed",servicesPassed)
                .put("system_server_services_log",new String(Files.readAllBytes(programLog("boot-services").toPath()),"UTF-8"));
            File frame=new File(root,"system-data/pocket-frame.png");Files.deleteIfExists(frame.toPath());
            java.lang.Process capture=startProgram(root,"screencap",new String[]{"-p","/data/pocket-frame.png"},"1000:1000","screencap");
            boolean captured=capture.waitFor(10,java.util.concurrent.TimeUnit.SECONDS) && capture.exitValue()==0;
            if (capture.isAlive()) capture.destroyForcibly();
            result.put("compositor_capture_passed",captured)
                .put("compositor_capture_log",new String(Files.readAllBytes(programLog("screencap").toPath()),"UTF-8"));
            if (captured) {
                Bitmap bitmap=BitmapFactory.decodeFile(frame.getPath());
                if (bitmap==null) throw new IOException("Compositor output is not a decodable PNG");
                result.put("compositor_width",bitmap.getWidth()).put("compositor_height",bitmap.getHeight());
                runOnUiThread(()->showFrame(bitmap));
            }
            String log=new String(Files.readAllBytes(programLog("zygote-init").toPath()),"UTF-8");
            boolean frameworkPassed=log.contains("DIRECT_FRAMEWORK_SMOKE_PASS launcher=1 install=1 activity=1 input=1");
            boolean complete=result.optBoolean("system_server_boot_completed") && frameworkPassed &&
                eglPassed && servicesPassed && captured && supervisor.isAlive();
            result.put("scope","system_server_bringup").put("system_server_log",log)
                .put("supervisor_alive",supervisor.isAlive()).put("framework_smoke_passed",frameworkPassed)
                .put("full_guest_boot_verified",complete).put("passed",complete);
            message(complete?"Android 9 boot, launcher, APK installation and input passed.":
                "System-server bring-up captured. Full boot remains unverified; inspect the startup log.");
        } finally {
            supervisor.destroy();
            if (!supervisor.waitFor(8,java.util.concurrent.TimeUnit.SECONDS)) supervisor.destroyForcibly();
        }
        return true;
    }
    @Override public void onDestroy() {
        destroyed=true;if (batteryRelay!=null) batteryRelay.close();super.onDestroy();
    }
    private void showFrame(Bitmap frame) {
        ViewGroup content=findViewById(android.R.id.content);
        View status=content.getChildAt(0);content.removeView(status);
        LinearLayout layout=new LinearLayout(this);layout.setOrientation(LinearLayout.VERTICAL);
        layout.addView(status,new LinearLayout.LayoutParams(-1,-2));
        ImageView image=new ImageView(this);image.setScaleType(ImageView.ScaleType.FIT_CENTER);image.setImageBitmap(frame);
        layout.addView(image,new LinearLayout.LayoutParams(-1,0,1));setContentView(layout);
    }
    private void patchInterpreter(File executable, String interpreter) throws Exception {
        byte[] elf = Files.readAllBytes(executable.toPath());
        if (elf.length < 64 || elf[0]!=127 || elf[1]!='E' || elf[2]!='L' || elf[3]!='F') return;
        ByteBuffer b = ByteBuffer.wrap(elf).order(ByteOrder.LITTLE_ENDIAN);
        int table=Math.toIntExact(b.getLong(32)), size=b.getShort(54)&65535, count=b.getShort(56)&65535;
        for (int n=0;n<count;++n) {
            int pos=table+n*size;
            if (b.getInt(pos)!=3) continue;
            int offset=Math.toIntExact(b.getLong(pos+8)), capacity=Math.toIntExact(b.getLong(pos+32));
            byte[] path=interpreter.getBytes("UTF-8");
            if (capacity < path.length+1 || offset<0 || capacity>elf.length-offset) throw new IOException("Invalid PT_INTERP reservation " + executable);
            Arrays.fill(elf,offset,offset+capacity,(byte)0);System.arraycopy(path,0,elf,offset,path.length);
            Files.write(executable.toPath(),elf); return;
        }
    }
    @Override protected File programLog(String name) {
        return new File(getFilesDir(), "api28-rootfs/logs/" + name + ".log");
    }
    @Override protected java.lang.Process startProgram(File root, String binary, String[] args, String identity, String output) throws IOException {
        try {
            if (destroyed) throw new IOException("Boot probe closed");
            if (output.equals("zygote-init")) {
                File data=new File(root,"data"), saved=new File(root,"foundation-data-"+System.nanoTime());
                if (!data.renameTo(saved)) throw new IOException("Cannot preserve foundation data");
                Os.symlink(systemServerMode?"system-data":"zygote-data",data.getPath());
                File console=new File(root,"pocket-console");Files.deleteIfExists(console.toPath());
                Os.symlink("logs/zygote-init.log",console.getPath());
            }
            File file = new File(root, "system/bin/" + binary);
            List<String> command=new ArrayList<>();command.add(file.getPath());Collections.addAll(command,args);
            ProcessBuilder builder=new ProcessBuilder(command).directory(root);
            Map<String,String> env=builder.environment();
            for (String name:new String[]{"LD_PRELOAD","LD_LIBRARY_PATH","BOOTCLASSPATH","DEX2OATBOOTCLASSPATH","SYSTEMSERVERCLASSPATH", "ANDROID_ART_ROOT","ANDROID_RUNTIME_ROOT","ANDROID_I18N_ROOT","ANDROID_TZDATA_ROOT"}) env.remove(name);
            env.put("POCKET_DIRECT_ROOT",root.getPath());
            env.put("POCKET_DIRECT_LIBRARIES",new File(root,"system/lib64").getPath()+":"+new File(root,"vendor/lib64").getPath());
            env.put("POCKET_DIRECT_ID",identity==null ? android.os.Process.myUid()+":"+android.os.Process.myUid() : identity);
            env.put("LD_LIBRARY_PATH",new File(root,"system/lib64").getPath()+":"+new File(root,"vendor/lib64").getPath());
            env.put("LD_CONFIG_FILE",new File(root,"system/etc/ld.config.txt").getPath());
            env.put("LD_PRELOAD",new File(root,"system/lib64/libpocket_direct_boot.so").getPath()+":"+new File(root,"system/lib64/libpocket_binder_api28.so").getPath()+":"+new File(root,"system/lib64/libpocket_hidl_api28.so").getPath());
            env.put("POCKET_PARENT_PID",Integer.toString(android.os.Process.myPid()));
            env.put("ANDROID_ROOT","/system");env.put("ANDROID_DATA","/data");env.put("TMPDIR","/tmp");
            if (output.startsWith("zygote")) env.put("POCKET_DIRECT_CONSOLE",programLog("zygote-init").getPath());
            File log=programLog(output); log.getParentFile().mkdirs();Files.write(log.toPath(),new byte[0]);
            builder.redirectInput(new File("/dev/null"));builder.redirectErrorStream(true);
            builder.redirectOutput(ProcessBuilder.Redirect.appendTo(log));
            return builder.start();
        } catch (Exception e) { throw new IOException("Direct launcher failed",e); }
    }
}
