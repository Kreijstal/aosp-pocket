// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.graphics.Bitmap;
import android.net.Uri;
import android.os.*;
import android.view.MotionEvent;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.regex.*;
import org.json.JSONObject;

/** Exercises the actual Manager service/session in a separate app sandbox. */
public final class ManagerProbe extends RuntimeActivity {
    private File root;
    private static void check(boolean value,String message)throws IOException {if(!value)throw new IOException(message);}
    private <T>T ui(Callable<T> action)throws Exception {
        FutureTask<T> task=new FutureTask<>(action);runOnUiThread(task);return task.get(30,TimeUnit.SECONDS);
    }
    @Override public void onCreate(Bundle state){
        getIntent().putExtra("guest_sdk",29);autoStart=true;super.onCreate(state);
        setShowWhenLocked(true);setTurnScreenOn(true);
        LinearLayout page=new LinearLayout(this);page.setOrientation(LinearLayout.VERTICAL);
        status=new TextView(this);page.addView(status);
        display=new GuestDisplay(this,false,true);page.addView(display,new LinearLayout.LayoutParams(-1,0,1));setContentView(page);
        root=DirectImage.root(this,29);
        new Thread(this::probe,"Api29ManagerValidation").start();
    }
    @Override public void onGuestChanged(){
        super.onGuestChanged();
        if(runtime!=null)try{Files.write(new File(getFilesDir(),"api29-manager-stage.txt").toPath(),runtime.status().getBytes("UTF-8"));}catch(IOException ignored){}
    }
    private String log()throws Exception{return new String(Files.readAllBytes(new File(root,"logs/manager-init.log").toPath()),"UTF-8");}
    private void boot()throws Exception {
        long deadline=SystemClock.elapsedRealtime()+900000;
        while(SystemClock.elapsedRealtime()<deadline){
            if(ui(()->runtime!=null && runtime.booted())){check(ui(()->runtime.selectedSdk())==29,"Manager selected the wrong SDK");return;}
            Thread.sleep(250);
        }
        throw new IOException("Manager did not boot Android 10");
    }
    private int frame(String name)throws Exception {
        long deadline=SystemClock.elapsedRealtime()+60000;
        while(SystemClock.elapsedRealtime()<deadline){
            Bitmap bitmap=ui(()->runtime.snapshot());
            if(bitmap!=null){
                Set<Integer> colors=new HashSet<>();
                for(int y=0;y<bitmap.getHeight();y+=11)for(int x=0;x<bitmap.getWidth();x+=11)colors.add(bitmap.getPixel(x,y));
                if(colors.size()>8){try(FileOutputStream output=new FileOutputStream(new File(getFilesDir(),name))){bitmap.compress(Bitmap.CompressFormat.PNG,100,output);}bitmap.recycle();return colors.size();}
                bitmap.recycle();
            }
            Thread.sleep(250);
        }
        throw new IOException("Manager did not receive a visible compositor frame");
    }
    private String await(String marker)throws Exception {
        long deadline=SystemClock.elapsedRealtime()+90000;
        while(SystemClock.elapsedRealtime()<deadline){String text=log();if(text.contains(marker))return text;Thread.sleep(250);}
        throw new IOException("Guest did not report "+marker);
    }
    private void stop()throws Exception {
        ui(()->{if(runtime!=null)runtime.stopGuest();return null;});
        long deadline=SystemClock.elapsedRealtime()+30000;
        while(ui(()->runtime!=null && (runtime.running() || runtime.stopping() || runtime.starting()))){
            if(SystemClock.elapsedRealtime()>deadline)throw new IOException("Manager did not stop its guest");Thread.sleep(100);
        }
    }
    private void probe(){
        JSONObject result=new JSONObject();
        File marker=new File(DirectImage.root(this,28),"system-data/api29-preservation-marker");
        try {
            marker.getParentFile().mkdirs();Files.write(marker.toPath(),"keep-android-nine".getBytes("UTF-8"));
            boot();result.put("selectedSdk",29).put("launcherColors",frame("api29-manager-launcher.png"));
            File apk=new File(getFilesDir(),"api29-smoke.apk");
            try(InputStream input=getAssets().open("api29-smoke.apk");OutputStream output=new FileOutputStream(apk)){GuestImage.copy(input,output,1024*1024);}
            Map<String,Uri> apks=Collections.singletonMap("base.apk",Uri.fromFile(apk));
            ui(()->{runtime.installApps(apks);return null;});
            String text=await("POCKET_GUEST_BUTTON center=");
            check(text.contains("DIRECT_FRAMEWORK_INSTALL_PASS"),"Manager install did not succeed");
            Matcher found=Pattern.compile("POCKET_GUEST_BUTTON center=([0-9]+),([0-9]+)").matcher(text);check(found.find(),"Test button coordinates missing");
            int x=Integer.parseInt(found.group(1)),y=Integer.parseInt(found.group(2));long down=SystemClock.uptimeMillis();
            for(int action:new int[]{MotionEvent.ACTION_DOWN,MotionEvent.ACTION_UP}){
                MotionEvent event=MotionEvent.obtain(down,SystemClock.uptimeMillis(),action,x,y,0);
                ui(()->{runtime.touch(event,1,1);return null;});event.recycle();Thread.sleep(40);
            }
            await("tag=PocketGuestTest tap=1");result.put("installAndInput",true).put("appColors",frame("api29-manager-app.png"));
            JSONObject timing=new JSONObject(new String(Files.readAllBytes(new File(root,"logs/display-performance.json").toPath()),"UTF-8"));
            check(timing.getLong("frames_presented")>0,"Manager never presented a frame to its host surface");result.put("display",timing);
            ui(()->{runtime.checkHostBridges();return null;});
            File network=new File(root,"logs/host-bridge-check.json");long until=SystemClock.elapsedRealtime()+45000;
            while(!network.isFile() && SystemClock.elapsedRealtime()<until)Thread.sleep(250);
            check(network.isFile(),"Guest network check did not finish");
            JSONObject online=new JSONObject(new String(Files.readAllBytes(network.toPath()),"UTF-8"));result.put("network",online);
            check(online.optBoolean("guest_network_connected") && online.optInt("http_status")==200,"Guest network/HTTPS check failed");
            stop();Files.write(new File(getFilesDir(),"api29-offline-ready").toPath(),new byte[]{1});
            until=SystemClock.elapsedRealtime()+60000;
            while(!new File(getFilesDir(),"api29-offline-go").isFile()){
                check(SystemClock.elapsedRealtime()<until,"Runner did not disable the image server");Thread.sleep(100);
            }
            ui(()->{runtime.startGuest();return null;});boot();result.put("offlineColors",frame("api29-manager-offline.png"));
            check(ui(()->runtime.hasLastApp()),"Installed app preference did not survive restart");
            ui(()->{runtime.openLastApp();return null;});await("POCKET_GUEST_BUTTON center=");
            check(new String(Files.readAllBytes(marker.toPath()),"UTF-8").equals("keep-android-nine"),"Android 9 data changed");
            result.put("offlineRestart",true).put("android9DataPreserved",true).put("validation","PASS");
        }catch(Throwable error){try{result.put("validation","FAIL").put("error",error.toString());}catch(Exception ignored){}}
        finally{try{stop();}catch(Exception error){try{result.put("validation","FAIL").put("cleanupError",error.toString());}catch(Exception ignored){}}}
        try{
            result.put("hostUid",android.os.Process.myUid());
            if(new File(root,"logs/manager-init.log").isFile())result.put("log",log());
            Files.write(new File(getFilesDir(),"api29-manager-result.json").toPath(),result.toString(2).getBytes("UTF-8"));
        }catch(Exception ignored){}
    }
}
