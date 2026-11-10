// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;
import android.os.Build;
import android.os.SystemClock;
import android.system.Os;
import android.system.OsConstants;
import java.io.*;
import java.lang.reflect.Method;
import java.nio.file.*;
import java.util.concurrent.*;

/** Exercise the actual framework-requested Java dump and interceptor FD path. */
public final class TombstonedProbe32 {
    private static void check(boolean ok,String reason){if(!ok)throw new AssertionError(reason);}
    private static boolean hasFrames(String text,int pid) {
        return text.contains("----- pid "+pid+" ") && text.contains("----- end "+pid+" -----") &&
            text.contains("TombstonedProbe32.main") && text.contains("pocket-requested-backtrace-wait") &&
            text.contains("java.lang.Thread.sleep");
    }
    public static void main(String[] args)throws Exception {
        check(Build.VERSION.SDK_INT==32 && Os.getuid()==0,"Expected isolated SDK32 root entry");
        check(args.length>=1,"Operation required");
        if("target".equals(args[0])) {
            Os.setgid(39999);Os.setuid(39999);
            Thread worker=new Thread(()->{try{Thread.sleep(600000);}catch(InterruptedException ignored){}},"pocket-requested-backtrace-wait");
            worker.start();
            Path ready=Paths.get(args[1]),temporary=Paths.get(args[1]+".new");
            Files.write(temporary,Integer.toString(Os.getpid()).getBytes("UTF-8"));
            Files.move(temporary,ready,StandardCopyOption.ATOMIC_MOVE);
            while(true)Thread.sleep(1000);
        }
        check("check".equals(args[0]),"Unknown operation");
        String nonce=java.util.UUID.randomUUID().toString();Path ready=Paths.get("/tmp/tombstoned-ready-"+nonce),output=Paths.get("/tmp/tombstoned-output-"+nonce);
        ProcessBuilder builder=new ProcessBuilder("/system/bin/pocket-app-process-api32","-Xnorelocate","-Xnoimage-dex2oat",
            "-Ximage:/apex/com.android.art/javalib/boot.art:/system/framework/boot-framework.art","-cp","/system/framework/pocket-tools.jar",
            "/system/bin",TombstonedProbe32.class.getName(),"target",ready.toString());
        builder.environment().put("POCKET_API32_USE_TOMBSTONED","1");
        builder.redirectErrorStream(true).redirectOutput(new File("/tmp/tombstoned-target.log"));
        Process target=builder.start();
        try {
            Os.setgid(1000);Os.setuid(1000);
            long until=SystemClock.uptimeMillis()+15000;
            while(!Files.exists(ready) && target.isAlive() && SystemClock.uptimeMillis()<until)Thread.sleep(50);
            check(Files.exists(ready) && target.isAlive(),"Trace target did not start");
            int pid=Integer.parseInt(new String(Files.readAllBytes(ready),"UTF-8"));
            Method dump=Class.forName("android.os.Debug").getDeclaredMethod("dumpJavaBacktraceToFileTimeout",int.class,String.class,int.class);
            dump.setAccessible(true);
            long began=SystemClock.uptimeMillis();
            boolean result=(Boolean)dump.invoke(null,pid,output.toString(),15);
            check(result,"Framework Java backtrace request failed");
            String text=new String(Files.readAllBytes(output),"UTF-8");
            check(hasFrames(text,pid),"Requested output missing actual target Java frames");
            check(target.isAlive(),"Backtrace unexpectedly terminated the target");
            System.out.println("API32_TOMBSTONED_JAVA_PASS requester_uid=1000 target_uid=39999 target_pid="+pid+
                " stock_Debug=1 intercept_fd=real ART_frames=verified target_alive=1 elapsed_ms="+(SystemClock.uptimeMillis()-began));
            // A second dump has no registered interceptor: stock tombstoned must
            // persist it in its own trace rotation, including on SELinux hosts
            // that disallow the daemon's O_TMPFILE hard-link operation.
            began=SystemClock.uptimeMillis();Os.kill(pid,OsConstants.SIGQUIT);
            Path saved=null;long bytes=0;until=began+15000;
            while(saved==null && target.isAlive() && SystemClock.uptimeMillis()<until) {
                try(DirectoryStream<Path> files=Files.newDirectoryStream(Paths.get("/data/anr"),"trace_*")) {
                    for(Path file:files) {
                        byte[] content=Files.readAllBytes(file);
                        if(hasFrames(new String(content,"UTF-8"),pid)){saved=file;bytes=content.length;break;}
                    }
                }
                if(saved==null)Thread.sleep(50);
            }
            check(saved!=null && target.isAlive(),"Standalone Java backtrace was not persisted with real target frames");
            System.out.println("API32_TOMBSTONED_PERSIST_PASS target_pid="+pid+" path="+saved+" bytes="+bytes+
                " ART_frames=verified target_alive=1 elapsed_ms="+(SystemClock.uptimeMillis()-began));
        }finally {
            target.destroy();if(!target.waitFor(3,TimeUnit.SECONDS)){target.destroyForcibly();target.waitFor();}
            Files.deleteIfExists(ready);Files.deleteIfExists(output);
        }
        System.exit(0);
    }
}
