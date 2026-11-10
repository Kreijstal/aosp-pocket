// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketav32test;

import android.app.Activity;
import android.content.pm.PackageManager;
import android.os.*;
import android.widget.TextView;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.JSONObject;
import org.json.JSONArray;

/** Public device APIs under an ordinary app UID; fixture permissions use stock PackageManager. */
public final class MainActivity extends Activity {
    private static volatile String snapshot="{}";
    private static final AtomicBoolean running=new AtomicBoolean();
    private static int generation;
    private TextView text;
    @Override public void dump(String prefix,java.io.FileDescriptor fd,java.io.PrintWriter out,String[] args) {
        out.println("POCKET_AV_APP32="+android.util.Base64.encodeToString(snapshot.getBytes(java.nio.charset.StandardCharsets.UTF_8),android.util.Base64.NO_WRAP));
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        text=new TextView(this);text.setTextSize(18);text.setPadding(24,24,24,24);setContentView(text);
    }
    @Override public void onResume() {
        super.onResume();if(!running.compareAndSet(false,true))return;
        JSONObject report=new JSONObject();
        try {
            if(Build.VERSION.SDK_INT!=32 || android.os.Process.myUid()<10000 || getApplicationInfo().uid!=android.os.Process.myUid())
                throw new IllegalStateException("An ordinary SDK32 app UID is required");
            report.put("sdk",32).put("uid",android.os.Process.myUid()).put("pid",android.os.Process.myPid())
                .put("package",getPackageName()).put("nonce",UUID.randomUUID().toString()).put("generation",++generation)
                .put("startedUptimeMillis",SystemClock.uptimeMillis()).put("validation","RUNNING").put("stage","permissions");
            snapshot=report.toString();text.setText("Running public audio and camera APIs");
        }catch(Exception error){running.set(false);throw new IllegalStateException(error);}
        new Thread(()->{
            try {
                long deadline=SystemClock.elapsedRealtime()+60000;
                while(checkSelfPermission(android.Manifest.permission.CAMERA)!=PackageManager.PERMISSION_GRANTED
                        || checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)!=PackageManager.PERMISSION_GRANTED) {
                    if(SystemClock.elapsedRealtime()>deadline)throw new IllegalStateException("Fixture runtime permissions were not granted");
                    Thread.sleep(100);
                }
                report.put("cameraPermission",true).put("microphonePermission",true);
                boolean passed=true;
                for(String stage:new String[]{"playback","microphone","camera","recorder"}) {
                    report.put("stage",stage);snapshot=report.toString();JSONObject result;
                    try {
                        if(stage.equals("recorder")) {
                            String[] ids=getSystemService(android.hardware.camera2.CameraManager.class).getCameraIdList();
                            if(ids.length==0)throw new IllegalStateException("No recording cameras advertised");
                            JSONArray recordings=new JSONArray();report.put("recordings",recordings);
                            for(String id:ids) {
                                JSONObject capture;
                                try{capture=CameraRecording.run(this,id).put("validation","PASS");}
                                catch(Throwable error){passed=false;java.io.StringWriter stack=new java.io.StringWriter();error.printStackTrace(new java.io.PrintWriter(stack));capture=new JSONObject().put("cameraId",id).put("validation","FAIL").put("error",error.toString()).put("errorStack",stack.toString());}
                                recordings.put(capture);snapshot=report.toString();
                            }
                            result=recordings.getJSONObject(0);
                        }else if(stage.equals("camera")) {
                            String[] ids=getSystemService(android.hardware.camera2.CameraManager.class).getCameraIdList();
                            if(ids.length==0)throw new IllegalStateException("No guest cameras advertised");
                            JSONArray cameras=new JSONArray();report.put("cameras",cameras);
                            for(String id:ids) {
                                JSONObject capture;
                                try{capture=DeviceWorkload.camera(this,id).put("validation","PASS");}
                                catch(Throwable error){passed=false;java.io.StringWriter stack=new java.io.StringWriter();error.printStackTrace(new java.io.PrintWriter(stack));capture=new JSONObject().put("cameraId",id).put("validation","FAIL").put("error",error.toString()).put("errorStack",stack.toString());}
                                cameras.put(capture);if(cameras.length()==1)report.put("camera",capture);
                                snapshot=report.toString();
                            }
                            result=cameras.getJSONObject(0);
                        } else {
                            result=(stage.equals("playback")?DeviceWorkload.playback():DeviceWorkload.microphone()).put("validation","PASS");
                        }
                    }catch(Throwable error){passed=false;java.io.StringWriter stack=new java.io.StringWriter();error.printStackTrace(new java.io.PrintWriter(stack));result=new JSONObject().put("validation","FAIL").put("error",error.toString()).put("errorStack",stack.toString());}
                    report.put(stage,result);snapshot=report.toString();
                }
                report.put("stage","complete").put("validation",passed?"PASS":"FAIL");
            }catch(Throwable error){try{report.put("validation","FAIL").put("error",error.toString());}catch(Exception ignored){}}
            finally {
                try{report.put("finishedUptimeMillis",SystemClock.uptimeMillis());}catch(Exception ignored){}
                snapshot=report.toString();running.set(false);runOnUiThread(()->text.setText(snapshot));
            }
        },"PublicAudioCamera32").start();
    }
}
