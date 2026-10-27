// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.loadertest;
import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
import java.io.*;
import java.nio.file.*;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

/** Isolated fixture package: never launches through Pocket's owner console. */
public final class MainActivity extends Activity {
    private TextView text;
    public void onCreate(Bundle state) {
        super.onCreate(state);text=new TextView(this);text.setText("Testing freestanding loader…");setContentView(text);
        new Thread(()->{
            JSONObject report=new JSONObject();
            try {
                File root=new File(getFilesDir(),"root");extract("root",root);
                Path link=new File(root,"fixture-link").toPath();Files.deleteIfExists(link);Files.createSymbolicLink(link,Paths.get("fixture.txt"));
                boolean boot=getIntent().getBooleanExtra("boot",false);
                boolean firewall=getIntent().getBooleanExtra("firewall",false);
                boolean denylist=getIntent().getBooleanExtra("denylist",false);
                if(!denylist)Files.deleteIfExists(new File(root,".pocket-module-view/ready").toPath());
                if(boot || firewall || denylist)for(String name:new String[]{".pocket-owner","dev/socket","dev/__properties__","dev/input","data","logs"}) {
                    File directory=new File(root,name);if(!directory.isDirectory() && !directory.mkdirs())throw new IOException("Cannot create "+name);
                }
                if(denylist) {
                    for(String name:new String[]{"system/etc","sbin/.magisk","data/adb/modules/test",".pocket-module-view/system/etc"})new File(root,name).mkdirs();
                    Files.write(new File(root,"system/etc/denylist-view").toPath(),"base".getBytes("UTF-8"));
                    Files.write(new File(root,".pocket-module-view/system/etc/denylist-view").toPath(),"overlay".getBytes("UTF-8"));
                    Files.write(new File(root,"system/bin/su").toPath(),"fixture".getBytes("UTF-8"));
                    Files.write(new File(root,"sbin/magisk").toPath(),"fixture".getBytes("UTF-8"));
                    Path alias=new File(root,"alias-su").toPath();Files.deleteIfExists(alias);Files.createSymbolicLink(alias,new File(root,"sbin/magisk").toPath());
                    for(String name:new String[]{"bin","lib64"}) {
                        Path view=new File(root,".pocket-module-view/system/"+name).toPath();Files.deleteIfExists(view);Files.createSymbolicLink(view,new File(root,"system/"+name).toPath());
                    }
                    Files.write(new File(root,".pocket-module-view/ready").toPath(),new byte[]{1});
                }
                if(boot) {
                    // Property areas are volatile per init lifetime. Touch only
                    // this fixture's own directory, without following links.
                    File[] stale=new File(root,"dev/__properties__").listFiles();
                    if(stale!=null)for(File entry:stale) {
                        if(Files.isSymbolicLink(entry.toPath()) || !entry.isFile())throw new IOException("Unexpected property entry");
                        Files.delete(entry.toPath());
                    }
                }
                File output=boot?new File(root,"logs/init.log"):new File(getFilesDir(),"native.log");
                ProcessBuilder builder=denylist?new ProcessBuilder(new File(root,"system/bin/denylist-probe").getPath(),root.getPath()).directory(root):new ProcessBuilder(new File(root,firewall?"system/bin/firewall-probe":boot?"system/bin/init-probe":"system/bin/probe").getPath()).directory(root);
                builder.environment().clear();if(boot || firewall || denylist) {
                    builder.environment().put("POCKET_LOADER_BOOT","1");
                    builder.environment().put("ANDROID_ROOT","/system");
                    builder.environment().put("ANDROID_DATA","/data");
                }
                builder.redirectInput(new File("/dev/null"));builder.redirectErrorStream(true);builder.redirectOutput(output);
                java.lang.Process process=builder.start();
                if(!process.waitFor(160,TimeUnit.SECONDS)){process.destroyForcibly();throw new IOException("Native loader timed out");}
                String log=new String(Files.readAllBytes(output.toPath()),"UTF-8");
                report.put("exitCode",process.exitValue()).put("log",log).put("hostUid",android.os.Process.myUid());
                boolean passed=process.exitValue()==0 && log.contains(denylist?"DENYLIST_VIEW_PASS":firewall?"NATIVE_FIREWALL_PASS":boot?"NATIVE_INIT_ZYGOTE_PASS":"LOADER_TEST_PASS");
                if(firewall) {
                    // The native test leaves guest OUTPUT denied. This host
                    // process must still be able to send its own datagrams.
                    try(java.net.DatagramSocket receiver=new java.net.DatagramSocket(0,java.net.InetAddress.getLoopbackAddress());
                        java.net.DatagramSocket sender=new java.net.DatagramSocket()) {
                        receiver.setSoTimeout(2000);byte[] data={42};
                        sender.send(new java.net.DatagramPacket(data,1,java.net.InetAddress.getLoopbackAddress(),receiver.getLocalPort()));
                        java.net.DatagramPacket packet=new java.net.DatagramPacket(new byte[1],1);receiver.receive(packet);
                        boolean hostOk=packet.getData()[0]==42;report.put("hostTrafficUnaffected",hostOk);passed=passed && hostOk;
                    }
                }
                report.put("passed",passed);
            }catch(Throwable error){try{report.put("passed",false).put("error",error.toString());}catch(Exception ignored){}}
            try{Files.write(new File(getFilesDir(),"report.json").toPath(),report.toString(2).getBytes("UTF-8"));}catch(Exception ignored){}
            final String display=report.toString();runOnUiThread(()->text.setText(display));
        },"PocketExecLoaderTest").start();
    }
    private void extract(String asset,File target)throws Exception {
        String[] entries=getAssets().list(asset);
        if(entries.length>0) {
            if(!target.isDirectory() && !target.mkdirs())throw new IOException("Cannot create fixture directory");
            for(String name:entries)extract(asset+"/"+name,new File(target,name));
        }else {
            if(Files.isSymbolicLink(target.toPath()))throw new IOException("Unexpected fixture symlink");
            try(InputStream in=getAssets().open(asset);OutputStream out=new FileOutputStream(target)){
                byte[] buffer=new byte[65536];int count;while((count=in.read(buffer))!=-1)out.write(buffer,0,count);
            }
            android.system.Os.chmod(target.getPath(),0755);
        }
    }
}
