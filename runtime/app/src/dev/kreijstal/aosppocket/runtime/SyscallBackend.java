// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.File;
import java.util.*;

/** Android 9 syscall translation. Bionic is neither preloaded nor patched. */
final class SyscallBackend {
    static List<String> command(File root,String cwd,List<String> program) throws java.io.IOException {
        if(program.isEmpty() || !program.get(0).startsWith("/"))throw new IllegalArgumentException("Guest executable must be absolute");
        File executable=directory(root,program.get(0));
        byte[] header=new byte[256];int count;
        try(java.io.InputStream in=new java.io.FileInputStream(executable)){count=in.read(header);}
        List<String> command=new ArrayList<>();
        if(count>=2 && header[0]=='#' && header[1]=='!') {
            String line=new String(header,0,count,java.nio.charset.StandardCharsets.UTF_8).split("\n",2)[0].trim();
            if(!line.equals("#!/system/bin/sh"))throw new java.io.IOException("Unsupported guest script interpreter: "+line);
            // Let the prepared guest shell read the virtual script pathname.
            // The host kernel must never select its own /system/bin/sh here.
            command.add(new File(root,"system/bin/sh").getPath());command.add(program.get(0));
            command.addAll(program.subList(1,program.size()));
        } else {
            if(count<4 || header[0]!=127 || header[1]!='E' || header[2]!='L' || header[3]!='F')
                throw new java.io.IOException("Guest executable is not ELF or an Android shell script");
            command.add(executable.getPath());command.addAll(program.subList(1,program.size()));
        }
        return command;
    }
    static File directory(File root,String cwd) throws java.io.IOException {
        if(!cwd.startsWith("/") || cwd.contains(".."))throw new java.io.IOException("Invalid guest working directory");
        String path=cwd.equals("/data")?"system-data":cwd.startsWith("/data/")?"system-data/"+cwd.substring(6):cwd.substring(1);
        File directory=new File(root,path);
        if(!directory.getCanonicalPath().startsWith(root.getCanonicalPath()+"/") && !directory.getCanonicalPath().equals(root.getCanonicalPath()))
            throw new java.io.IOException("Working directory escapes image");
        return directory;
    }
    static Map<String,String> environment(File root,int parentPid) {
        return environment(root,parentPid,28);
    }
    static Map<String,String> environment(File root,int parentPid,int sdk) {
        if(sdk!=28 && sdk!=29)throw new IllegalArgumentException("Unsupported direct guest SDK");
        Map<String,String> env=new LinkedHashMap<>();
        env.put("POCKET_LOADER_BOOT","1");
        env.put("POCKET_SYSCALL_BACKEND","native");
        env.put("LD_PRELOAD","/system/lib64/libpocket_binder_api"+sdk+".so:/system/lib64/libpocket_hidl_api"+sdk+".so");
        if(sdk==29){
            env.put("ANDROID_RUNTIME_ROOT","/apex/com.android.runtime");
            env.put("ANDROID_TZDATA_ROOT","/apex/com.android.tzdata");
            java.util.Properties exports=new java.util.Properties();
            try(java.io.Reader reader=new java.io.InputStreamReader(new java.io.FileInputStream(new File(root,"system/etc/pocket-api29.env")),"UTF-8")){
                exports.load(reader);
            }catch(java.io.IOException error){throw new IllegalStateException("Cannot load Android 10 runtime environment",error);}
            for(String key:exports.stringPropertyNames()){
                if(!key.matches("ANDROID_[A-Z0-9_]+|BOOTCLASSPATH|DEX2OATBOOTCLASSPATH|SYSTEMSERVERCLASSPATH|EXTERNAL_STORAGE|ASEC_MOUNTPOINT"))
                    throw new IllegalStateException("Invalid Android 10 environment variable");
                env.put(key,exports.getProperty(key));
            }
        }
        env.put("POCKET_PARENT_PID",Integer.toString(parentPid));
        env.put("POCKET_GUEST_CONSOLE","/logs/manager-init.log");
        env.put("PATH","/system/bin:/system/xbin:/vendor/bin");
        env.put("ANDROID_ROOT","/system");env.put("ANDROID_DATA","/data");
        env.put("ANDROID_ASSETS","/system/app");env.put("ANDROID_STORAGE","/storage");
        env.put("ASEC_MOUNTPOINT","/mnt/asec");env.put("EXTERNAL_STORAGE","/sdcard");
        env.put("TMPDIR","/data/local/tmp");env.put("HOME","/data/local/tmp");
        env.put("USER","root");env.put("LOGNAME","root");
        env.put("SHELL","/system/bin/sh");env.put("TERM","xterm-256color");
        return env;
    }
}
