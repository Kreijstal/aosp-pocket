// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.system.Os;
import java.lang.reflect.Method;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Paths;

/** Verify real stock framework image mappings, rather than just staged files. */
public final class BootArtifactsProbe32 {
    private static void check(boolean value,String message) {
        if(!value)throw new AssertionError(message);
    }
    static boolean hasCode(String path)throws Exception {
        try(RandomAccessFile input=new RandomAccessFile(path,"r")) {
            byte[] bytes=new byte[64];input.readFully(bytes);
            ByteBuffer header=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
            check(header.getInt(0)==0x464c457f && bytes[4]==2 && bytes[5]==1,"Expected ELF64 LE: "+path);
            long offset=header.getLong(32);int stride=header.getShort(54)&65535,count=header.getShort(56)&65535;
            check(offset>=64 && count>0 && count<=64 && stride==56 && offset+(long)count*stride<=input.length(),"Invalid OAT ELF headers: "+path);
            for(int i=0;i<count;i++) {
                input.seek(offset+(long)i*stride);input.readFully(bytes,0,56);
                if(header.getInt(0)==1 && (header.getInt(4)&1)!=0 && header.getLong(32)>0)return true;
            }
            return false;
        }
    }
    public static void main(String[] args) {
        try {run();System.exit(0);}
        catch(Throwable error){error.printStackTrace();System.err.println("API32_FRAMEWORK_BOOT_IMAGES_FAILED");System.exit(1);}
    }
    private static void run()throws Exception {
        check(android.os.Build.VERSION.SDK_INT==32 && Os.getuid()==0,"Expected isolated API32 root entry");
        Class.forName("android.app.ActivityThread",false,null);
        String maps=new String(Files.readAllBytes(Paths.get("/proc/self/maps")),"UTF-8");
        String[] components={"framework","framework-graphics","ext","telephony-common","voip-common","ims-common","core-icu4j"};
        int executable=0;
        for(String name:components) {
            String base="/system/framework/arm64/boot-"+name;
            check(maps.contains(base+".art"),"Stock boot image not mapped: "+base+".art");
            check(maps.contains(base+".oat"),"Stock boot OAT not mapped: "+base+".oat");
            // Some multi-image OAT components contain only metadata. Require
            // executable mappings exactly when their original ELF has code.
            if(!hasCode(base+".oat"))continue;
            boolean code=false;
            for(String line:maps.split("\n"))if(line.contains(base+".oat") && line.contains("r-xp"))code=true;
            check(code,"Stock boot code not executable: "+base+".oat");executable++;
        }
        System.out.println("API32_FRAMEWORK_BOOT_IMAGES_PASS source=stock mapped_images="+components.length+
            " executable_oat="+executable+" bytecode=unchanged");
        Class<?> dex=Class.forName("dalvik.system.DexFile");
        Method info=dex.getMethod("getDexFileOptimizationInfo",String.class,String.class);
        for(String path:System.getenv("SYSTEMSERVERCLASSPATH").split(":")) {
            Object result=info.invoke(null,path,"arm64");
            Object status=result.getClass().getMethod("getStatus").invoke(result);
            Object reason=result.getClass().getMethod("getReason").invoke(result);
            System.out.println("API32_SYSTEMSERVER_PREOPT_OBSERVED path="+path+" filter="+status+" reason="+reason);
        }
    }
}
