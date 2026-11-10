// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.graphics.Bitmap;
import android.net.Uri;
import android.os.*;
import android.widget.*;
import java.io.*;
import java.nio.file.Files;
import java.util.*;
import java.util.concurrent.*;
import java.util.regex.Pattern;
import org.json.JSONObject;

/** Installs an unchanged external APK through Manager in its isolated API 29 fixture. */
public final class AppProbe extends RuntimeActivity {
    private File root;
    private <T>T ui(Callable<T> action)throws Exception {
        FutureTask<T> task=new FutureTask<>(action);runOnUiThread(task);return task.get(30,TimeUnit.SECONDS);
    }
    @Override public void onCreate(Bundle state) {
        getIntent().putExtra("guest_sdk",29);autoStart=true;super.onCreate(state);
        setShowWhenLocked(true);setTurnScreenOn(true);
        LinearLayout page=new LinearLayout(this);page.setOrientation(LinearLayout.VERTICAL);
        status=new TextView(this);page.addView(status);
        display=new GuestDisplay(this,false,true);page.addView(display,new LinearLayout.LayoutParams(-1,0,1));setContentView(page);
        root=DirectImage.root(this,29);new Thread(this::probe,"PocketAppCompatibility").start();
    }
    private String guest(String... argv)throws Exception {
        java.lang.Process process=DirectImage.admin(root,"/",Arrays.asList(argv));
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();
        Thread reader=new Thread(()->{try(InputStream in=process.getInputStream()){GuestImage.copy(in,bytes,4*1024*1024);}catch(IOException ignored){}});
        Thread errors=new Thread(()->{try(InputStream in=process.getErrorStream()){GuestImage.copy(in,new ByteArrayOutputStream(),4*1024*1024);}catch(IOException ignored){}});
        reader.start();errors.start();
        if(!process.waitFor(40,TimeUnit.SECONDS)){process.destroyForcibly();throw new IOException("Guest command timed out");}
        reader.join(2000);errors.join(2000);
        return bytes.toString("UTF-8");
    }
    private void probe() {
        JSONObject report=new JSONObject();
        try {
            long deadline=SystemClock.elapsedRealtime()+300000;
            while(!ui(()->runtime!=null && runtime.booted())) {
                if(SystemClock.elapsedRealtime()>deadline)throw new IOException("Guest boot timed out");Thread.sleep(250);
            }
            String name=getIntent().getStringExtra("app_package");
            File apk=new File(getFilesDir(),"compatibility.apk");
            if(name==null || !name.matches("[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z][A-Za-z0-9_]*)+") || !apk.isFile())
                throw new IOException("Stage compatibility.apk and supply app_package");
            report.put("guestSdk",29).put("package",name).put("apkBytes",apk.length());
            ui(()->{runtime.installApps(Collections.singletonMap("base.apk",Uri.fromFile(apk)));return null;});
            deadline=SystemClock.elapsedRealtime()+180000;
            while(true) {
                String state=ui(()->runtime.status());
                if(state.equals("Installed "+name))break;
                if(state.contains("operation failed") || SystemClock.elapsedRealtime()>deadline)throw new IOException(state);
                Thread.sleep(250);
            }
            Thread.sleep(20000);
            String activities=guest("/system/bin/dumpsys","activity","activities");
            report.put("activities",activities);
            String processes=guest("/system/bin/dumpsys","activity","processes");report.put("processes",processes);
            String pid=guest("/system/bin/toybox","pidof",name).trim();report.put("appPid",pid);
            boolean resumed=Pattern.compile("mResumedActivity:.*?"+Pattern.quote(name)+"/").matcher(activities).find();
            report.put("resumedActivity",resumed);
            Bitmap bitmap=ui(()->runtime.snapshot());
            int colors=0;
            if(bitmap!=null){
                Set<Integer> sampled=new HashSet<>();
                for(int y=0;y<bitmap.getHeight();y+=11)for(int x=0;x<bitmap.getWidth();x+=11)sampled.add(bitmap.getPixel(x,y));
                colors=sampled.size();
                try(OutputStream output=new FileOutputStream(new File(getFilesDir(),"compatibility.png"))){bitmap.compress(Bitmap.CompressFormat.PNG,100,output);}bitmap.recycle();
            }
            report.put("sampledColors",colors).put("installation","PASS");
            report.put("launchValidated",resumed && pid.matches("[0-9]+(?: [0-9]+)*") && colors>8);
            report.put("scope","APK installation and foreground startup only; account registration, linking, messaging and calls untested");
        } catch(Throwable error){try{report.put("error",error.toString());}catch(Exception ignored){}}
        finally {
            try {ui(()->{if(runtime!=null)runtime.stopGuest();return null;});}
            catch(Exception error){try{report.put("cleanupError",error.toString());}catch(Exception ignored){}}
        }
        try {Files.write(new File(getFilesDir(),"compatibility-result.json").toPath(),report.toString(2).getBytes("UTF-8"));}
        catch(Exception ignored){}
    }
}
