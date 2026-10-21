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
                File output=new File(getFilesDir(),"native.log");
                ProcessBuilder builder=new ProcessBuilder(new File(root,"system/bin/probe").getPath()).directory(root);
                builder.environment().clear();builder.redirectErrorStream(true);builder.redirectOutput(output);
                java.lang.Process process=builder.start();
                if(!process.waitFor(75,TimeUnit.SECONDS)){process.destroyForcibly();throw new IOException("Native loader timed out");}
                String log=new String(Files.readAllBytes(output.toPath()),"UTF-8");
                report.put("exitCode",process.exitValue()).put("log",log).put("hostUid",android.os.Process.myUid());
                report.put("passed",process.exitValue()==0 && log.contains("LOADER_TEST_PASS"));
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
