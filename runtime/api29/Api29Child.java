// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;
import java.nio.file.Files;
import java.nio.file.Paths;

/** Loaded from the boot extension after a real API 29 ZygoteConnection fork. */
public final class Api29Child {
    public static void main(String[] args)throws Exception {
        int uid=android.system.Os.getuid(),gid=android.system.Os.getgid();
        if(uid!=39999 || gid!=39999 || android.os.Build.VERSION.SDK_INT!=29 || args.length!=1)
            throw new AssertionError("Wrong Android 10 child credentials or framework");
        final int[] result={0};
        Thread thread=new Thread(()->result[0]=29,"api29-child-check");
        thread.start();thread.join();
        if(result[0]!=29)throw new AssertionError("Child thread failed");
        String report="API29_JAVA_CHILD_PASS uid="+uid+" gid="+gid+" sdk=29 threads=1 nonce="+args[0]+"\n";
        Files.write(Paths.get("/data/api29-child.txt"),report.getBytes("UTF-8"));
        System.out.print(report);
    }
}
