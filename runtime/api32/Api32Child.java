// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Arrays;

/** Runs after a real API 32 ZygoteConnection fork and native specialization. */
public final class Api32Child {
    private static native int[] groups();
    private static native int kernelUid();
    private static native void rawProcLines(String path,String[] fields,long[] values);
    private static void processIdentity()throws Exception {
        java.lang.reflect.Method read=android.os.Process.class.getMethod("readProcLines",String.class,String[].class,long[].class);
        java.lang.reflect.Method uid=android.os.Process.class.getMethod("getUidForPid",int.class);
        int pid=android.os.Process.myPid(),parent=android.system.Os.getppid();
        String status="/proc/"+pid+"/status";
        String[] fields={"Uid:","PPid:","Tgid:","Threads:"};
        long[] raw={-1,-1,-1,-1},guest={-1,-1,-1,-1};
        rawProcLines(status,fields,raw);read.invoke(null,status,fields,guest);
        if(kernelUid()<10000 || raw[0]!=kernelUid() || guest[0]!=39999 ||
                raw[1]!=guest[1] || raw[2]!=guest[2] || raw[3]!=guest[3] ||
                guest[1]!=parent || guest[2]!=pid || guest[3]<1 ||
                ((Integer)uid.invoke(null,pid))!=39999 || ((Integer)uid.invoke(null,parent))!=0)
            throw new AssertionError("Guest process UID or original kernel measurements lost");
        // Other host processes retain the original result, including a denied
        // or unavailable /proc file. Ordinary files with a Uid label do too.
        for(String path:new String[]{"/proc/1/status","/proc/2147483647/status","/data/api32-proc-label.txt"}) {
            if(path.endsWith(".txt"))Files.write(Paths.get(path),"Uid:\t1234\nPPid:\t42\n".getBytes("UTF-8"));
            long[] before={-1,-1},after={-1,-1};String[] labels={"Uid:","PPid:"};
            rawProcLines(path,labels,before);read.invoke(null,path,labels,after);
            if(!Arrays.equals(before,after))throw new AssertionError("Non-guest process/file was changed");
        }
        Files.delete(Paths.get("/data/api32-proc-label.txt"));
        try {
            read.invoke(null,status,new String[]{"Uid:","PPid:"},new long[1]);
            throw new AssertionError("Platform array validation lost");
        }catch(java.lang.reflect.InvocationTargetException expected) {
            if(!(expected.getCause() instanceof IllegalArgumentException))throw expected;
        }
        System.out.println("API32_PROCESS_IDENTITY_PASS self_uid=guest parent_uid=guest proc_measurements=stock host_uid=actual host_process=unchanged invalid_array=rejected");
    }
    public static void main(String[] args)throws Exception {
        int uid=android.system.Os.getuid(),gid=android.system.Os.getgid();
        int[] groups=groups();
        if(uid!=39999 || gid!=39999 || android.os.Build.VERSION.SDK_INT!=32 || args.length!=1 ||
                !Arrays.equals(groups,new int[]{1000,3003}) ||
                !Arrays.asList(android.os.Build.SUPPORTED_64_BIT_ABIS).contains("arm64-v8a"))
            throw new AssertionError("Wrong 12L child credentials, groups, ABI or framework");
        int priority=android.os.Process.getThreadPriority(android.os.Process.myTid());
        if(priority!=0)throw new AssertionError("Child lost the fixture's host priority: "+priority);
        processIdentity();
        final int[] result={0};
        Thread thread=new Thread(()->result[0]=32,"api32-child-check");
        thread.start();thread.join();
        if(result[0]!=32)throw new AssertionError("Child thread failed");
        if(java.security.Security.getProvider("AndroidKeyStore")==null ||
            java.security.Security.getProvider("AndroidKeyStoreBCWorkaround")==null)
            throw new AssertionError("12L child did not inherit JCA providers");
        byte[] plain="Android 12L crypto".getBytes("UTF-8");
        javax.crypto.SecretKey key=new javax.crypto.spec.SecretKeySpec(new byte[16],"AES");
        javax.crypto.Cipher encrypt=javax.crypto.Cipher.getInstance("AES/GCM/NoPadding");
        encrypt.init(javax.crypto.Cipher.ENCRYPT_MODE,key);
        byte[] encrypted=encrypt.doFinal(plain);
        javax.crypto.Cipher decrypt=javax.crypto.Cipher.getInstance("AES/GCM/NoPadding");
        decrypt.init(javax.crypto.Cipher.DECRYPT_MODE,key,new javax.crypto.spec.GCMParameterSpec(128,encrypt.getIV()));
        if(!Arrays.equals(plain,decrypt.doFinal(encrypted)))throw new AssertionError("12L AES-GCM round trip failed");
        if(!(new android.graphics.Paint().measureText("Android 12L")>0))
            throw new AssertionError("12L child font measurement failed");
        String report="API32_JAVA_CHILD_PASS uid="+uid+" gid="+gid+" sdk=32 threads=1 groups=1000,3003 nice="+priority+" crypto=aes_gcm fonts=1 nonce="+args[0]+"\n";
        Files.write(Paths.get("/data/api32-child.txt"),report.getBytes("UTF-8"));
        System.out.print(report);
    }
}
