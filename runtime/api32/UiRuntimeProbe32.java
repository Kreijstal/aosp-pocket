// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.app.ActivityManager;
import android.content.*;
import android.content.pm.*;
import android.os.*;
import android.system.Os;
import java.io.*;
import java.lang.reflect.*;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.concurrent.*;
import org.json.JSONObject;

/** Real stock UI dexopt and bounded, observable Home/Settings readiness. */
public final class UiRuntimeProbe32 {
    private static void check(boolean ok,String reason){if(!ok)throw new AssertionError(reason);}
    private static Object activity,task;
    private static Class<?> activityApi,taskApi;
    private static String hash(File file)throws Exception {
        if(!file.isFile())return "absent";
        MessageDigest digest=MessageDigest.getInstance("SHA-256");
        try(InputStream input=new FileInputStream(file)) {
            byte[] buffer=new byte[65536];int size;while((size=input.read(buffer))!=-1)digest.update(buffer,0,size);
        }
        StringBuilder value=new StringBuilder();for(byte b:digest.digest())value.append(String.format(Locale.ROOT,"%02x",b&255));
        return value.toString();
    }
    private static byte[] prefix(File file,int limit)throws Exception {
        try(InputStream input=new FileInputStream(file);ByteArrayOutputStream output=new ByteArrayOutputStream()) {
            byte[] buffer=new byte[8192];int size;
            while(output.size()<limit && (size=input.read(buffer,0,Math.min(buffer.length,limit-output.size())))!=-1)output.write(buffer,0,size);
            return output.toByteArray();
        }
    }
    private static void compile(Context context)throws Exception {
        String pkg="com.android.systemui";
        ApplicationInfo app=context.getPackageManager().getApplicationInfo(pkg,0);
        check(app.uid>=10000 && app.sourceDir.startsWith("/system_ext/"),"Unexpected stock SystemUI package");
        check(!new File(app.sourceDir).getParentFile().canWrite(),"SystemUI source directory must be read-only for ART cache selection");
        File oat=new File("/data/dalvik-cache/arm64/"+app.sourceDir.substring(1).replace('/','@')+"@classes.dex");
        String before=hash(oat);long modified=oat.lastModified(),started=SystemClock.uptimeMillis();
        Object manager=Class.forName("android.app.AppGlobals").getMethod("getPackageManager").invoke(null);
        boolean result=(Boolean)Class.forName("android.content.pm.IPackageManager")
            .getMethod("performDexOptMode",String.class,boolean.class,String.class,boolean.class,boolean.class,String.class)
            .invoke(manager,pkg,false,"speed",false,true,null);
        check(result,"Stock PackageManager speed dexopt failed");
        byte[] bytes=prefix(oat,131072);
        check(bytes.length>64 && bytes[0]==127 && bytes[1]=='E' && bytes[2]=='L' && bytes[3]=='F' && bytes[4]==2 && bytes[5]==1,
            "SystemUI cache is not a 64-bit little-endian ELF");
        check(new String(bytes,"ISO-8859-1").contains("compiler-filter\0speed\0"),"SystemUI cache does not declare the requested speed filter");
        String after=hash(oat);boolean reused=before.equals(after) && modified==oat.lastModified();
        System.out.println("API32_UI_DEXOPT_PASS package="+pkg+" source_parent_writable=false compiler=stock_dex2oat filter=speed bytes="+oat.length()+
            " sha256="+after+" reused="+reused+" elapsed_ms="+(SystemClock.uptimeMillis()-started));
    }
    private static int systemUiPid(Context context)throws Exception {
        int uid=context.getPackageManager().getApplicationInfo("com.android.systemui",0).uid;
        @SuppressWarnings("unchecked") List<ActivityManager.RunningAppProcessInfo> list=(List<ActivityManager.RunningAppProcessInfo>)
            activityApi.getMethod("getRunningAppProcesses").invoke(activity);
        if(list!=null)for(ActivityManager.RunningAppProcessInfo info:list)
            if("com.android.systemui".equals(info.processName) && info.uid==uid && info.pid>0)return info.pid;
        return -1;
    }
    private static void verifyCode(Context context,int pid,String stage)throws Exception {
        ApplicationInfo app=context.getPackageManager().getApplicationInfo("com.android.systemui",0);
        String cached="/data/dalvik-cache/arm64/"+app.sourceDir.substring(1).replace('/','@')+"@classes.dex";
        String adjacent=new File(app.sourceDir).getParent()+"/oat/arm64/"+
            new File(app.sourceDir).getName().replace(".apk",".odex");
        String command=new String(Files.readAllBytes(Paths.get("/proc/"+pid+"/cmdline")),"UTF-8");
        check(command.startsWith("com.android.systemui\0"),"SystemUI PID no longer names the expected process");
        String maps=new String(Files.readAllBytes(Paths.get("/proc/"+pid+"/maps")),"UTF-8"),selected=null;
        StringBuilder diagnostic=new StringBuilder();
        for(String line:maps.split("\n")) {
            if(line.contains("SystemUI") || line.contains("system_ext@priv-app@"))diagnostic.append(line).append('\n');
            if(line.contains("r-xp") && (line.endsWith(cached) || line.endsWith(adjacent)))selected=line.endsWith(cached)?cached:adjacent;
        }
        check(selected!=null,"Running SystemUI has no executable app OAT mapping:\n"+diagnostic);
        byte[] header=prefix(new File(selected),131072);
        check(header.length>64 && header[0]==127 && header[1]=='E' && header[2]=='L' && header[3]=='F' &&
            new String(header,"ISO-8859-1").contains("compiler-filter\0speed\0"),
            "Running SystemUI did not load speed-compiled code: "+selected);
        check(systemUiPid(context)==pid,"SystemUI PID changed while checking executable code");
        System.out.println("API32_SYSTEMUI_CODE_LOADED_PASS stage="+stage+" pid="+pid+" path="+selected+
            " filter=speed executable_mapping=1 source="+(selected.equals(cached)?"app_data_cache":"platform_preopt"));
    }
    private static ComponentName focused()throws Exception {
        Object info=taskApi.getMethod("getFocusedRootTaskInfo").invoke(task);
        return info==null?null:(ComponentName)info.getClass().getField("topActivity").get(info);
    }
    private static String windowDump()throws Exception {
        IBinder binder=(IBinder)Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,"window");
        check(binder!=null,"WindowManager missing");File file=new File("/tmp/ui32-windows.txt");
        try(FileOutputStream output=new FileOutputStream(file)){binder.dump(output.getFD(),new String[]{"windows"});}
        String text=new String(prefix(file,512*1024),"UTF-8");
        check(!text.contains("Permission Denial:"),"Stock WindowManager DUMP denied the system role");
        return text;
    }
    private static boolean drawn(String dump,ComponentName name) {
        String[] windows=dump.split("(?m)^\\s*Window #");
        for(String window:windows) {
            int end=window.indexOf('\n');String title=end<0?window:window.substring(0,end);
            if((title.contains(name.flattenToString()) || title.contains(name.flattenToShortString())) &&
                window.contains("mHasSurface=true") && window.contains("isReadyForDisplay()=true") &&
                window.contains("mDrawState=HAS_DRAWN") && window.contains("isOnScreen=true"))return true;
        }
        return false;
    }
    private static ComponentName launch(Context context,Intent intent)throws Exception {
        ResolveInfo resolved=context.getPackageManager().resolveActivity(intent,PackageManager.MATCH_DEFAULT_ONLY);
        check(resolved!=null && resolved.activityInfo!=null,"No stock activity resolves "+intent);
        ComponentName target=new ComponentName(resolved.activityInfo.packageName,resolved.activityInfo.name);
        check(!"android".equals(target.getPackageName()),"UI intent resolved to a chooser instead of an application");
        intent.setComponent(target).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        Method method=null;
        for(Method candidate:taskApi.getMethods())if(candidate.getName().equals("startActivityAsUser") &&
            candidate.getParameterCount()==12 && candidate.getParameterTypes()[3]==Intent.class)method=candidate;
        check(method!=null,"SDK32 activity start ABI missing");
        int result=(Integer)method.invoke(task,null,"android",null,intent,null,null,null,0,0,null,null,0);
        check(result>=0,"UI activity start failed: "+result);return target;
    }
    private static void phase(Context context,String phase,Intent intent,int uiPid)throws Exception {
        Files.deleteIfExists(Paths.get("/tmp/ui32-phase.json"));
        ComponentName target=launch(context,intent);long deadline=SystemClock.uptimeMillis()+45000;String dump="";
        while(SystemClock.uptimeMillis()<deadline) {
            check(systemUiPid(context)==uiPid,"SystemUI exited/restarted while opening "+phase);
            if(target.equals(focused())) {
                dump=windowDump();
                if(drawn(dump,target))break;
            }
            Thread.sleep(500);
        }
        check(target.equals(focused()) && drawn(dump,target),"UI window did not draw: "+target+"\n"+dump);
        JSONObject ready=new JSONObject().put("runId",System.getenv("POCKET_API32_RUN_ID"))
            .put("phase",phase).put("package",target.getPackageName()).put("activity",target.getClassName())
            .put("systemUiPid",uiPid).put("windowDrawn",true);
        Path file=Paths.get("/tmp/ui32-phase.json"),temporary=Paths.get("/tmp/ui32-phase.json.new");
        Files.write(temporary,ready.toString().getBytes("UTF-8"));Files.move(temporary,file,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
        System.out.println("API32_UI_PHASE_READY phase="+phase+" activity="+target.flattenToShortString()+" systemui_pid="+uiPid+" drawn=stock_WindowManager");
        long end=SystemClock.uptimeMillis()+20000;
        while(SystemClock.uptimeMillis()<end) {
            check(systemUiPid(context)==uiPid,"SystemUI exited/restarted during "+phase);
            check(target.equals(focused()),"UI activity lost focus during "+phase);Thread.sleep(500);
        }
    }
    private static void verify(Context context)throws Exception {
        SystemServerDexopt32.verifyLoaded();
        activityApi=Class.forName("android.app.IActivityManager");activity=Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
        taskApi=Class.forName("android.app.IActivityTaskManager");task=Class.forName("android.app.ActivityTaskManager").getMethod("getService").invoke(null);
        int pid=systemUiPid(context);check(pid>0,"Stock SystemUI process is absent");
        try {
            verifyCode(context,pid,"before");
            phase(context,"home",new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_HOME),pid);
            phase(context,"settings",new Intent(android.provider.Settings.ACTION_SETTINGS),pid);
            verifyCode(context,pid,"after");
            System.out.println("API32_UI_RUNTIME_PASS home=focused_drawn settings=focused_drawn systemui_pid="+pid+" stable_seconds=40");
        }finally{Files.deleteIfExists(Paths.get("/tmp/ui32-phase.json"));}
    }
    public static void main(String[] args) {
        try {
            check(Build.VERSION.SDK_INT==32 && Os.getuid()==0 && args.length==1,"Expected SDK32 root entry and UI operation");
            Os.setgid(1000);Os.setuid(1000);Looper.prepare();
            Object thread=Class.forName("android.app.ActivityThread").getMethod("systemMain").invoke(null);
            Context context=(Context)thread.getClass().getMethod("getSystemContext").invoke(thread);
            FutureTask<Void> work=new FutureTask<>(()->{
                if("compile".equals(args[0]))compile(context);
                else if("verify".equals(args[0]))verify(context);
                else throw new IllegalArgumentException("Unknown UI operation");return null;
            });
            Thread worker=new Thread(work,"Api32UiRuntime");worker.setDaemon(true);worker.start();work.get(180,TimeUnit.SECONDS);
            System.exit(0);
        }catch(Throwable error){error.printStackTrace();System.err.println("API32_UI_RUNTIME_FAILED");System.exit(1);}
    }
}
