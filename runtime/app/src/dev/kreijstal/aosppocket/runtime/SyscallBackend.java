// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.File;
import java.util.*;

/** Android 9 syscall translation. Bionic is neither preloaded nor patched. */
final class SyscallBackend {
    private static final String PLATFORM = "LD_PRELOAD=/system/lib64/libpocket_binder_api28.so:/system/lib64/libpocket_hidl_api28.so";
    static List<String> command(File root,String cwd,List<String> program) {
        List<String> command=new ArrayList<>(Arrays.asList(
            new File(root,"system/bin/pocket-syscall-tracer").getPath(),"--kill-on-exit",
            "-r",root.getPath(),"-0","-w",cwd,
            "-b","/dev","-b","/proc","-b","/sys",
            "-b",new File(root,"system-data").getPath()+":/data",
            "-b",new File(root,"dev/socket").getPath()+":/dev/socket",
            "-b",new File(root,"dev/__properties__").getPath()+":/dev/__properties__",
            "-b",new File(root,"dev/input").getPath()+":/dev/input",
            "-b","/dev/binder:/dev/hwbinder",
            "/system/bin/toybox","env",PLATFORM));
        // Never bind the physical root at its host app-data pathname: Android
        // may otherwise mistake it for an obsolete guest app and delete it.
        command.addAll(program);
        return command;
    }
    static Map<String,String> environment(File root,int parentPid) {
        Map<String,String> env=new LinkedHashMap<>();
        env.put("PROOT_NO_SECCOMP","1");
        env.put("PROOT_TMP_DIR",new File(root.getParentFile(),"syscall-tmp").getPath());
        env.put("POCKET_PROOT_OWNER","1");
        env.put("POCKET_SYSCALL_BACKEND","proot");
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
