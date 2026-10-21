// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.File;
import java.util.*;

public final class SyscallBackendTest {
    private static void check(boolean value,String message) {
        if(!value)throw new AssertionError(message);
    }
    public static void main(String[] args) {
        File root=new File("/data/user/0/dev.kreijstal.aosppocket.runtime/files/api28-rootfs");
        List<String> program=Arrays.asList("/system/bin/sh","-c","printf '%s' '$() literal'");
        List<String> command=SyscallBackend.command(root,"/data/local/tmp",program);
        check(command.subList(command.size()-program.size(),command.size()).equals(program),"Program arguments changed");
        Set<String> allowed=new HashSet<>(Arrays.asList("/dev","/proc","/sys","/data","/dev/socket",
            "/dev/__properties__","/dev/input","/dev/hwbinder"));
        for(int i=0;i<command.size();i++) {
            check(!command.get(i).contains("libpocket_direct_boot"),"Bionic wrapper in command");
            if(!command.get(i).equals("-b"))continue;
            String bind=command.get(++i);
            String target=bind.substring(bind.lastIndexOf(':')+1);
            check(allowed.contains(target),"Unexpected bind target: "+target);
            check(!target.startsWith(root.getPath()),"Host root exposed in guest app data");
        }
        Map<String,String> env=SyscallBackend.environment(root,123);
        check(!env.containsKey("LD_PRELOAD") && !env.containsKey("LD_LIBRARY_PATH"),"Guest libraries leaked into host tracer");
        check(env.get("POCKET_PROOT_OWNER").equals("1"),"Ownership backend missing");
        check(env.get("ASEC_MOUNTPOINT").equals("/mnt/asec"),"Installd environment missing");
        check(env.get("POCKET_GUEST_CONSOLE").equals("/logs/manager-init.log"),"Console path mismatch");
        System.out.println("PASS: syscall command keeps guest arguments, binds only intended guest paths, omits Bionic wrappers and provides Android environment");
    }
}
