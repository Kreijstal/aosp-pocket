// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.*;

/** Exercises the stock AndroidRuntime registration and 12L observer JNI. */
public final class UEventProbe {
    private static native int inject(byte[] record);
    private static Method method(Class<?> type,String name,Class<?>... args)throws Exception {
        Method result=type.getDeclaredMethod(name,args);result.setAccessible(true);return result;
    }
    private static void send(String path)throws Exception {
        byte[] record=("change@"+path+"\0ACTION=change\0DEVPATH="+path+"\0SUBSYSTEM=pocket-test\0")
            .getBytes(StandardCharsets.US_ASCII);
        if(inject(record)!=record.length)throw new AssertionError("Private device event injection failed");
    }
    private static FutureTask<String> receive(Method wait) {
        FutureTask<String> task=new FutureTask<>(()->(String)wait.invoke(null));
        Thread thread=new Thread(task,"api32-uevent-wait");thread.setDaemon(true);thread.start();return task;
    }
    private static void expectBlocked(FutureTask<String> task)throws Exception {
        try {task.get(100,TimeUnit.MILLISECONDS);throw new AssertionError("Unmatched event was delivered");}
        catch(TimeoutException expected){}
    }
    public static void main(String[] args)throws Exception {
        if(android.os.Build.VERSION.SDK_INT!=32)throw new AssertionError("Wrong platform");
        System.load("/system/lib64/libpocket_uevent_api32.so");
        Class<?> observer=Class.forName("android.os.UEventObserver");
        Method setup=method(observer,"nativeSetup"),wait=method(observer,"nativeWaitForNextEvent");
        Method add=method(observer,"nativeAddMatch",String.class),remove=method(observer,"nativeRemoveMatch",String.class);
        setup.invoke(null);
        String first="/devices/pocket-api32-test",second="/devices/pocket-api32-next";
        add.invoke(null,"DEVPATH="+first);
        FutureTask<String> event=receive(wait);send(second);expectBlocked(event);send(first);
        String record=event.get(3,TimeUnit.SECONDS);
        Class<?> parsedType=Class.forName("android.os.UEventObserver$UEvent");
        Object parsed=parsedType.getConstructor(String.class).newInstance(record);
        Method get=parsedType.getMethod("get",String.class);
        if(!"change".equals(get.invoke(parsed,"ACTION")) || !first.equals(get.invoke(parsed,"DEVPATH")))
            throw new AssertionError("Stock event parser lost fields");
        remove.invoke(null,"DEVPATH="+first);add.invoke(null,"DEVPATH="+second);
        event=receive(wait);send(first);expectBlocked(event);send(second);
        if(!event.get(3,TimeUnit.SECONDS).contains("DEVPATH="+second))throw new AssertionError("Match removal failed");
        remove.invoke(null,"DEVPATH="+second);
        System.out.println("API32_UEVENT_JNI_PASS stock_setup=1 matching=1 removal=1 parsed_fields=1 host_netlink=unused source=synthetic_test");
    }
}
