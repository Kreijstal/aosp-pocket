// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;
import java.io.File;
import java.nio.file.Files;
import java.util.concurrent.CountDownLatch;

/** Confirms a real ART dump contains live Java frames and a blocked thread. */
public final class TraceProbe32 {
    public static void main(String[] args)throws Exception {
        if(android.os.Build.VERSION.SDK_INT!=32)throw new AssertionError("Wrong platform");
        int pid=android.os.Process.myPid();CountDownLatch waiting=new CountDownLatch(1);
        Thread worker=new Thread(()->{waiting.countDown();try{Thread.sleep(10000);}catch(InterruptedException expected){}},"pocket-api32-trace-wait");
        worker.start();waiting.await();
        android.os.Process.class.getMethod("sendSignal",int.class,int.class).invoke(null,pid,3);
        String text=null;File found=null;
        for(int attempt=0;attempt<100 && text==null;attempt++) {
            File[] files=new File("/tmp/art-traces").listFiles();
            if(files!=null)for(File file:files)if(file.getName().startsWith("anr_"+pid+"_")) {
                String value=new String(Files.readAllBytes(file.toPath()),"UTF-8");
                if(value.contains("----- end "+pid+" -----")){text=value;found=file;break;}
            }
            if(text==null)Thread.sleep(50);
        }
        worker.interrupt();worker.join(1000);
        if(text==null || !text.contains("\"main\"") || !text.contains("TraceProbe32.main") ||
            !text.contains("pocket-api32-trace-wait") || !text.contains("java.lang.Thread.sleep"))
            throw new AssertionError("Actual ART stack dump missing Java frames");
        Files.delete(found.toPath());
        System.out.println("API32_ART_TRACE_PASS stock_ART_sigquit=1 java_stack=real file_sink=private probe_cleanup=1");
        System.exit(0);
    }
}
