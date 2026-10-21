// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;
import java.io.*;
/** Runs only after a real Android 9 ZygoteConnection fork request. */
public final class LoaderChild {
    public static void main(String[] args) throws Exception {
        Class<?> os=Class.forName("android.system.Os");
        int uid=(Integer)os.getMethod("getuid").invoke(null);
        int gid=(Integer)os.getMethod("getgid").invoke(null);
        if(uid!=39999 || gid!=39999)throw new AssertionError("Guest credentials not inherited");
        final int[] answer={0};Thread thread=new Thread(()->answer[0]=28,"loader-child-test");thread.start();thread.join();
        if(answer[0]!=28)throw new AssertionError("Thread failed");
        try(Writer output=new FileWriter("/data/loader-child.txt")) {
            output.write("NATIVE_JAVA_CHILD_PASS uid="+uid+" gid="+gid+" threads=1\n");
        }
    }
}
