// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.os.SystemClock;
import android.system.Os;
import java.io.*;
import java.lang.reflect.Method;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.concurrent.*;

/** Select trusted platform preopts; prepare APEX verification through stock ART. */
public final class SystemServerDexopt32 {
    private static void check(boolean ok,String reason){if(!ok)throw new AssertionError(reason);}
    private static String hash(File file)throws Exception {
        if(!file.isFile())return "absent";
        MessageDigest digest=MessageDigest.getInstance("SHA-256");
        try(InputStream input=new FileInputStream(file)) {
            byte[] bytes=new byte[65536];int count;while((count=input.read(bytes))!=-1)digest.update(bytes,0,count);
        }
        StringBuilder value=new StringBuilder();for(byte b:digest.digest())value.append(String.format(Locale.ROOT,"%02x",b&255));
        return value.toString();
    }
    private static String filter(Class<?> dex,String path)throws Exception {
        Object info=dex.getMethod("getDexFileOptimizationInfo",String.class,String.class).invoke(null,path,"arm64");
        return (String)info.getClass().getMethod("getStatus").invoke(info);
    }
    private static int invalidate(File cache)throws Exception {
        int removed=0;String path=cache.getPath();
        for(String suffix:new String[]{"dex","vdex","art"}) {
            Path item=Paths.get(path.substring(0,path.length()-3)+suffix);
            check(!Files.isSymbolicLink(item),"Unexpected derived-cache symlink: "+item);
            if(Files.deleteIfExists(item))removed++;
        }
        return removed;
    }
    private static void prepare()throws Exception {
        String target=(String)Class.forName("android.os.SystemProperties").getMethod("get",String.class)
            .invoke(null,"dalvik.vm.systemservercompilerfilter");
        check("verify".equals(target),"APEX service code must retain stock verification policy");
        String classPath=System.getenv("SYSTEMSERVERCLASSPATH");check(classPath!=null && !classPath.isEmpty(),"System-server classpath missing");
        check(System.getenv("BOOTCLASSPATH").endsWith(":/system/framework/pocket-zygote.jar"),"Preparation/zygote boot classpaths must match");
        String[] paths=classPath.split(":");int count=paths.length;
        Class<?> dex=Class.forName("dalvik.system.DexFile");
        Method needed=dex.getMethod("getDexOptNeeded",String.class,String.class,String.class,String.class,boolean.class,boolean.class);
        String[] contexts=new String[count],before=new String[count],hashes=new String[count],required=new String[count];
        File[] artifacts=new File[count];long[] modified=new long[count];int[] prior=new int[count],removed=new int[count];
        String preceding="";int system=0,apex=0;
        for(int i=0;i<count;i++) {
            String path=paths[i];check((path.startsWith("/system/framework/") || path.startsWith("/apex/")) && new File(path).isFile(),"Invalid platform JAR: "+path);
            check(!new File(path).getParentFile().canWrite(),"System-server JAR parent must be read-only: "+path);
            contexts[i]="PCL["+preceding+"]";preceding+= (preceding.isEmpty()?"":":")+path;
            before[i]=filter(dex,path);
            File cache=new File("/data/dalvik-cache/arm64/"+path.substring(1).replace('/','@')+"@classes.dex");
            if(path.startsWith("/system/framework/")) {
                // Old fixtures produced data caches while these platform
                // preopts were absent. Invalidate only these disposable
                // service caches; the immutable source JARs are unchanged.
                removed[i]=invalidate(cache);system++;required[i]="speed";
                artifacts[i]=new File("/system/framework/oat/arm64/"+new File(path).getName().replace(".jar",".odex"));
                check(artifacts[i].isFile() && !artifacts[i].canWrite() && BootArtifactsProbe32.hasCode(artifacts[i].getPath()),"Trusted system preopt missing: "+path);
                check("speed".equals(filter(dex,path)),"ART did not select the trusted system preopt: "+path);
            }else {
                apex++;required[i]="verify";artifacts[i]=cache;
                // Recover earlier experimental AOT caches. Stock system_server
                // rejects executable artifacts in ordinary dalvik-cache.
                if(!"verify".equals(before[i]))removed[i]=invalidate(cache);
            }
            prior[i]=(Integer)needed.invoke(null,path,"arm64",required[i],contexts[i],false,false);
            hashes[i]=hash(artifacts[i]);modified[i]=artifacts[i].lastModified();
        }
        long started=SystemClock.uptimeMillis();
        Method stock=Class.forName("com.android.internal.os.ZygoteInit").getDeclaredMethod("performSystemServerDexOpt",String.class);
        stock.setAccessible(true);stock.invoke(null,classPath);
        int compiled=0;
        for(int i=0;i<count;i++) {
            int remaining=(Integer)needed.invoke(null,paths[i],"arm64",required[i],contexts[i],false,false);
            String after=filter(dex,paths[i]);check(remaining==0 && required[i].equals(after),"Service JAR did not select its required trusted filter: "+paths[i]+" filter="+after+" needed="+remaining);
            if(prior[i]!=0)compiled++;
            String current=hash(artifacts[i]);boolean reused=current.equals(hashes[i]) && modified[i]==artifacts[i].lastModified();
            System.out.println("API32_SYSTEMSERVER_DEXOPT_ITEM path="+paths[i]+" before="+before[i]+" after="+after+
                " needed_before="+prior[i]+" needed_after="+remaining+" invalidated_cache_files="+removed[i]+" artifact="+artifacts[i].getPath()+
                " artifact_bytes="+artifacts[i].length()+" artifact_sha256="+current+" artifact_reused="+reused);
        }
        System.out.println("API32_SYSTEMSERVER_DEXOPT_PASS stock_zygote_dexopt=1 system_prebuilt_speed="+system+" apex_verify="+apex+" paths="+count+
            " compiled="+compiled+" reused="+(count-compiled)+" elapsed_ms="+(SystemClock.uptimeMillis()-started));
    }
    static void verifyLoaded()throws Exception {
        int[] pids=(int[])android.os.Process.class.getMethod("getPidsForCommands",String[].class)
            .invoke(null,(Object)new String[]{"system_server"});
        check(pids!=null && pids.length==1 && pids[0]>0,"Expected exactly one accessible guest system_server");
        String maps=new String(Files.readAllBytes(Paths.get("/proc/"+pids[0]+"/maps")),"UTF-8");
        String stock="/system/framework/oat/arm64/services.odex";String selected=null;
        for(String line:maps.split("\n"))if(line.contains("r-xp") && line.endsWith(stock))selected=stock;
        check(selected!=null,"system_server has no executable trusted services.jar OAT mapping");
        byte[] bytes=new byte[131072];int length=0;
        try(FileInputStream input=new FileInputStream(selected)) {
            int count;while(length<bytes.length && (count=input.read(bytes,length,bytes.length-length))!=-1)length+=count;
        }
        check(length>0 && new String(bytes,0,length,"ISO-8859-1").contains("compiler-filter\0speed\0"),"Loaded service OAT does not declare speed");
        System.out.println("API32_SYSTEMSERVER_CODE_LOADED_PASS pid="+pids[0]+" path="+selected+" filter=speed executable_mapping=1");
    }
    public static void main(String[] args) {
        try {
            check(android.os.Build.VERSION.SDK_INT==32 && Os.getuid()==0,"Expected isolated SDK32 root entry");
            Os.setgid(1000);Os.setuid(1000);
            FutureTask<Void> work=new FutureTask<>(()->{prepare();return null;});
            Thread worker=new Thread(work,"Api32SystemServerDexopt");worker.setDaemon(true);worker.start();work.get(240,TimeUnit.SECONDS);
            System.exit(0);
        }catch(Throwable error){error.printStackTrace();System.err.println("API32_SYSTEMSERVER_DEXOPT_FAILED");System.exit(1);}
    }
}
