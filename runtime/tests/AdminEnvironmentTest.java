// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.File;
import java.util.*;

public final class AdminEnvironmentTest {
    private static void check(boolean value,String message) {
        if(!value)throw new AssertionError(message);
    }
    public static void main(String[] args) throws Exception {
        File root=new File("/data/user/0/dev.kreijstal.aosppocket.api32managerimage/files/api32");
        int pid=Integer.parseInt(new File("/proc/self").getCanonicalFile().getName());
        Map<String,String> boot=SyscallBackend.environment(root,pid,32);
        Map<String,String> admin=SyscallBackend.adminEnvironment(root,pid,32);
        // Boot scripts choose their own preloads; the shared boot map stays clean.
        check(!boot.containsKey("LD_PRELOAD"),"12L boot environment gained a preload");
        check(SyscallBackend.API32_BINDER_PRELOAD.equals(admin.get("LD_PRELOAD")),"12L admin programs lack the Binder Parcel adapter");
        Map<String,String> rest=new LinkedHashMap<>(admin);rest.remove("LD_PRELOAD");
        check(rest.equals(boot),"12L admin environment differs from boot beyond the preload");
        // Android 9 keeps its existing shared preload unchanged.
        check(SyscallBackend.adminEnvironment(root,pid,28).equals(SyscallBackend.environment(root,pid,28)),"SDK 28 admin environment changed");
        System.out.println("PASS: 12L admin programs preload the Binder Parcel adapter; boot and older SDK environments unchanged");
    }
}
