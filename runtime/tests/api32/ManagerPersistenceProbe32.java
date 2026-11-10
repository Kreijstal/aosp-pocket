// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.graphics.Bitmap;
import android.net.Uri;
import android.os.*;
import android.view.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.function.Predicate;
import org.json.*;

/** Two real guest boots across an outer-Manager process restart, with image server offline. */
public final class ManagerPersistenceProbe32 extends ManagerSessionProbe32 {
    private static final String PREFIX="PocketPersist32:STATE ";
    private JSONObject checkpoint;
    private boolean offline;
    private JSONObject latestState()throws Exception {
        JSONObject last=null;
        for(String line:nativeLog().split("\n")) {
            int offset=line.indexOf(PREFIX);if(offset<0)continue;
            last=new JSONObject(line.substring(offset+PREFIX.length()));
        }
        return last;
    }
    private JSONObject awaitState(Predicate<JSONObject> predicate)throws Exception {
        long until=SystemClock.elapsedRealtime()+90000;
        while(SystemClock.elapsedRealtime()<until) {
            JSONObject state=latestState();
            if(state!=null) {
                if(state.has("error"))throw new IOException("Ordinary SDK32 app: "+state.getString("error"));
                if(state.optBoolean("cryptoValid") && state.optBoolean("drawn") && predicate.test(state))return state;
            }
            File log=new File(root,"logs/manager-operation.log");
            if(log.isFile() && log.length()<2*1024*1024) {
                String text=new String(Files.readAllBytes(log.toPath()),"UTF-8");
                if(text.contains("POCKET_MANAGER32_ERROR"))throw new IOException(text.substring(0,Math.min(2000,text.length())));
            }
            Thread.sleep(250);
        }
        throw new IOException("Ordinary SDK32 app did not report the expected state");
    }
    private void target(JSONObject state,String kind,String stage)throws Exception {
        JSONObject position=ui(()->{
            View surface=display.getChildAt(0);int[] location=new int[2];surface.getLocationOnScreen(location);
            check(surface.isShown() && surface.getWidth()>0 && surface.getHeight()>0,"Host input surface is not visible");
            return new JSONObject().put("x",location[0]+state.getInt(kind+"X")*surface.getWidth()/runtime.width)
                .put("y",location[1]+state.getInt(kind+"Y")*surface.getHeight()/runtime.height)
                .put("id",(offline?"offline":"first")+":"+state.getString("activityId")+":"+kind);
        });
        result.put("hostInputTarget",position);stage(stage);
    }
    private JSONObject captureCycle()throws Exception {
        stop();JSONObject session=session(true);String logs=session.optString("nativeLog");session.remove("nativeLog");
        check(session.has("ownerExitCode") && session.getInt("ownerExitCode")==0 && !session.optBoolean("ownerAlive",true)
            && !session.has("cleanupFailure"),"Guest supervisor did not stop cleanly");
        check(logs.length()<2*1024*1024,"Guest native-log capture limit reached");
        return new JSONObject().put("session",session).put("console",console()).put("nativeLog",logs);
    }
    @Override protected void probe() {
        try {
            offline=getIntent().getBooleanExtra("offline_restart",false);
            result.put("runId",runId).put("probeKind","persistence").put("phase",offline?"offline":"first")
                .put("hostPid",android.os.Process.myPid());stage("booting");
            File saved=new File(getFilesDir(),"persistence32-checkpoint.json");
            if(offline) {
                checkpoint=new JSONObject(new String(Files.readAllBytes(saved.toPath()),"UTF-8"));
                check(checkpoint.getString("runId").equals(runId),"Wrong persistence checkpoint");
                check(checkpoint.getInt("hostPid")!=android.os.Process.myPid(),"Outer Manager process did not restart");
            }
            boot();checkCrashTransport();result.put("homeColors",frame("persist32-home-"+(offline?"offline":"first")+".png"));
            Files.deleteIfExists(new File(root,"logs/manager-operation.log").toPath());
            if(!offline) {
                File apk=new File(getFilesDir(),"persistence32.apk");
                try(InputStream in=getAssets().open("api32-persistence.apk");OutputStream out=new FileOutputStream(apk)){GuestImage.copy(in,out,1024*1024);}
                stage("installing");ui(()->{runtime.installApps(Collections.singletonMap("base.apk",Uri.fromFile(apk)));return null;});
            }else {
                check(ui(()->runtime.hasLastApp()),"Last installed app preference did not survive Manager restart");
                ui(()->{runtime.openLastApp();return null;});
            }
            JSONObject before=awaitState(s->s.optInt("sdk")==32 && s.optInt("uid")>=10000);
            result.put("initialApp",before);
            if(offline) {
                JSONObject previous=checkpoint.getJSONObject("app");
                check(before.getString("token").equals(previous.getString("token")) && before.getString("cipherDigest").equals(previous.getString("cipherDigest"))
                    && before.getString("text").equals(previous.getString("text")) && before.getInt("taps")==previous.getInt("taps")
                    && before.getInt("uid")==previous.getInt("uid") && before.getInt("pid")!=previous.getInt("pid")
                    && before.getLong("launches")>previous.getLong("launches"),"Private data, package UID or keystore did not persist");
                result.put("persistenceVerified",true);
            }
            int expectedTaps=before.getInt("taps")+1;String token=before.getString("token");
            target(before,"button","awaiting-host-tap");
            JSONObject tapped=awaitState(s->token.equals(s.optString("token")) && s.optInt("taps")==expectedTaps);
            JSONObject typed=tapped;
            if(!offline) {
                String text="p"+runId.substring(0,7);result.put("hostText",text);
                target(tapped,"editor","awaiting-host-text");typed=awaitState(s->text.equals(s.optString("text")) && token.equals(s.optString("token")));
            }
            long previousResumes=typed.getLong("resumes");
            ui(()->{runtime.key(KeyEvent.KEYCODE_HOME);return null;});Thread.sleep(1000);
            Files.deleteIfExists(new File(root,"logs/manager-operation.log").toPath());
            ui(()->{runtime.openLastApp();return null;});
            JSONObject reopened=awaitState(s->token.equals(s.optString("token")) && s.optLong("resumes")>previousResumes && s.optInt("taps")==expectedTaps);
            result.put("reopenVerified",true).put("appColors",frame("persist32-app-"+(offline?"offline":"first")+".png"));
            stage("observing");Thread.sleep(40000);
            File perf=new File(root,"logs/display-performance.json");check(perf.isFile(),"No display presentation report");
            JSONObject frames=new JSONObject(new String(Files.readAllBytes(perf.toPath()),"UTF-8"));check(frames.getLong("frames_presented")>0,"No frames presented on host surface");
            stage("stopping");JSONObject cycle=captureCycle().put("display",frames).put("app",reopened)
                .put("crashRoleDenied",result.getString("tombstoned_denied")).put("crashOutsideDenied",result.getString("tombstoned_outside"));
            JSONObject input=cycle.getJSONObject("session").getJSONObject("input");
            check(input.getLong("inputEventsRejected")==0 && input.getLong("touchEventsAccepted")>=(offline?2:4)
                && input.getLong("keyEventsAccepted")>=2,"Input did not reach the guest across the lifecycle");
            if(!offline) {
                JSONObject data=new JSONObject().put("runId",runId).put("hostPid",android.os.Process.myPid()).put("app",reopened).put("cycle",cycle);
                Files.write(saved.toPath(),data.toString().getBytes("UTF-8"));
                result.put("checkpoint",data);stage("awaiting-host-restart");return;
            }
            result.put("cycles",new JSONArray().put(checkpoint.getJSONObject("cycle")).put(cycle)).put("functionalPassed",true)
                .put("hostProcessRestarted",true).put("validation","PASS");stage("finished");finished=result.toString();
        }catch(Throwable error) {
            try {
                result.put("functionalPassed",false).put("validation","FAIL").put("error",error.toString());
                try{result.put("failedCycle",captureCycle());}catch(Exception cleanup){result.put("cleanupError",cleanup.toString());}
                if(checkpoint!=null)result.put("firstCycle",checkpoint.getJSONObject("cycle"));
                result.put("stage","finished");finished=result.toString();
            }catch(Exception ignored){}
        }
        if(finished!=null)try{Files.write(new File(getFilesDir(),"persistence32-result.json").toPath(),finished.getBytes("UTF-8"));}catch(IOException ignored){}
    }
}
