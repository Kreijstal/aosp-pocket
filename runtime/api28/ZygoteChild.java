// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;
import java.io.*;

/** Executes only after the real ZygoteServer accepts a fork command. */
public final class ZygoteChild {
    public static void main(String[] args) throws Exception {
        Class<?> process = Class.forName("android.os.Process");
        int pid = (Integer)process.getMethod("myPid").invoke(null);
        int uid = (Integer)process.getMethod("myUid").invoke(null);
        String sdk = (String)Class.forName("android.os.SystemProperties")
            .getMethod("get", String.class).invoke(null, "ro.build.version.sdk");
        Class<?> os = Class.forName("android.system.Os");
        if ((Integer)os.getMethod("getgid").invoke(null) != 39999) throw new AssertionError("Wrong guest GID");
        boolean denied = false;
        try { os.getMethod("setuid", int.class).invoke(null, 0); }
        catch (java.lang.reflect.InvocationTargetException expected) {
            Throwable cause = expected.getCause();
            denied = cause.getClass().getName().equals("android.system.ErrnoException") &&
                cause.getClass().getField("errno").getInt(cause) == 1;
        }
        if (!denied || (Integer)process.getMethod("myUid").invoke(null) != 39999)
            throw new AssertionError("Dropped guest UID could regain root");
        Process binder = new ProcessBuilder("/system/bin/pocket-api28-probe", "client-virtual")
            .redirectErrorStream(true).start();
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        byte[] buffer = new byte[1024];
        for (int count; (count = binder.getInputStream().read(buffer)) != -1;) bytes.write(buffer, 0, count);
        String binderOutput = bytes.toString("UTF-8");
        if (binder.waitFor() != 0 || !binderOutput.contains("server_uid=0 client_uid=39999"))
            throw new AssertionError("Forked child Binder path failed: " + binderOutput);
        boolean appProfile=args.length==2 && args[1].equals("app");
        if (appProfile) {
            Process groupProbe=new ProcessBuilder("/system/bin/pocket-boot-probe","app").redirectErrorStream(true).start();
            ByteArrayOutputStream groupBytes=new ByteArrayOutputStream();
            for (int count;(count=groupProbe.getInputStream().read(buffer))!=-1;) groupBytes.write(buffer,0,count);
            if (groupProbe.waitFor()!=0 || !groupBytes.toString("UTF-8").contains("DIRECT_APP_GROUPS_PASS"))
                throw new AssertionError("Wrong app guest groups: "+groupBytes.toString("UTF-8"));
        }
        final int[] answer = {0};
        Thread child = new Thread(() -> answer[0] = 28, "pocket-zygote-child");
        child.start(); child.join();
        if (uid != 39999 || !sdk.equals("28") || answer[0] != 28 || (!appProfile && args.length != 1))
            throw new AssertionError("Bad zygote child identity or runtime");
        try (Writer out = new OutputStreamWriter(new FileOutputStream("/data/pocket-zygote-child.json"), "UTF-8")) {
            out.write("{\"app_profile\":"+appProfile+",\"passed\":true,\"pid\":" + pid + ",\"uid\":" + uid + ",\"sdk\":28,\"nonce\":\"" + args[0] + "\",\"thread\":28,\"binder\":true,\"regain_root_denied\":true}\n");
        }
        // Remain alive for independent inspection and explicit termination/reaping.
        Thread.sleep(120000);
    }
}
