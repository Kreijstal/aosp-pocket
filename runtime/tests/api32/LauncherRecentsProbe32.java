// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;
import android.os.Build;
import android.os.RemoteException;
import android.system.Os;
import dalvik.system.DexClassLoader;
import java.io.File;
import java.lang.reflect.*;
import java.util.ArrayList;
import org.json.JSONObject;

/** Actual Android ART fault injection for original and patched Launcher DEX. */
public final class LauncherRecentsProbe32 {
    private static void check(boolean value,String why){if(!value)throw new AssertionError(why);}
    private static Object allocate(Class<?> type)throws Exception {
        Class<?> unsafe=Class.forName("sun.misc.Unsafe");Field field=unsafe.getDeclaredField("theUnsafe");field.setAccessible(true);
        return unsafe.getMethod("allocateInstance",Class.class).invoke(field.get(null),type);
    }
    private static Object call(Method method,Object proxy)throws Throwable {
        try{return method.invoke(proxy,7,0);}catch(InvocationTargetException e){throw e.getCause();}
    }
    private static final class Peer implements InvocationHandler {
        Object response;Throwable failure;int calls;
        public Object invoke(Object proxy,Method method,Object[] args)throws Throwable {
            check(method.getName().equals("getRecentTasks"),"Unexpected service method: "+method);
            check(args.length==3 && args[0].equals(7) && args[1].equals(2) && args[2].equals(0),"Task query arguments changed");
            calls++;if(failure!=null)throw failure;return response;
        }
    }
    private static JSONObject run(String apk,String cache,boolean patched)throws Throwable {
        File directory=new File(cache);check(directory.mkdir(),"Use a fresh optimized-code directory");
        ClassLoader loader=new DexClassLoader(apk,cache,null,LauncherRecentsProbe32.class.getClassLoader());
        Class<?> type=Class.forName("com.android.quickstep.SystemUiProxy",true,loader);
        check(type.getClassLoader()==loader,"Launcher was resolved by the parent loader");
        Object target=allocate(type);Field service=type.getDeclaredField("mRecentTasks");service.setAccessible(true);
        Method method=type.getMethod("getRecentTasks",int.class,int.class);
        Object result=call(method,target);check(result instanceof ArrayList && ((ArrayList<?>)result).isEmpty(),"Disconnected service no longer returns empty");
        Class<?> api=loader.loadClass("com.android.wm.shell.recents.IRecentTasks");Peer peer=new Peer();
        service.set(target,Proxy.newProxyInstance(loader,new Class<?>[]{api},peer));
        boolean nullRejected=false;
        try{result=call(method,target);check(patched && result instanceof ArrayList && ((ArrayList<?>)result).isEmpty(),"Null-array result was not handled");}
        catch(NullPointerException expected){check(!patched,"Patched method still crashes on null");nullRejected=true;}
        check(nullRejected!=patched,"Original versus patched control failed");
        Class<?> task=loader.loadClass("com.android.wm.shell.util.GroupedRecentTaskInfo");
        peer.response=Array.newInstance(task,0);result=call(method,target);
        check(result instanceof ArrayList && ((ArrayList<?>)result).isEmpty(),"Empty typed array changed");
        Object first=allocate(task),second=allocate(task);Object tasks=Array.newInstance(task,2);Array.set(tasks,0,first);Array.set(tasks,1,second);peer.response=tasks;
        result=call(method,target);check(result instanceof ArrayList && ((ArrayList<?>)result).size()==2
            && ((ArrayList<?>)result).get(0)==first && ((ArrayList<?>)result).get(1)==second,"Non-null tasks were lost/replaced/reordered");
        peer.failure=new RemoteException("controlled-recents-remote-failure");result=call(method,target);
        check(result instanceof ArrayList && ((ArrayList<?>)result).isEmpty(),"Existing RemoteException fallback changed");
        for(Throwable error:new Throwable[]{new SecurityException("controlled-recents-permission-denial"),new IllegalStateException("controlled-recents-state-error")}) {
            peer.failure=error;boolean rejected=false;
            try{call(method,target);}catch(Throwable observed){check(observed==error,"Error identity or type changed");rejected=true;}
            check(rejected,"An error was suppressed");
        }
        check(peer.calls==6,"Unexpected service call count");
        return new JSONObject().put("patched",patched).put("loader",type.getClassLoader().getClass().getName())
            .put("originalNullNpeReproduced",!patched && nullRejected).put("nullArrayEmptyFallbackVerified",patched && !nullRejected)
            .put("disconnectedAndEmptyFallbacksVerified",true).put("nonnullTaskIdentityAndOrderPreserved",true)
            .put("remoteExceptionFallbackPreserved",true).put("securityAndStateErrorsPreserved",true).put("queryArgumentsPreserved",true)
            .put("serviceCalls",peer.calls);
    }
    public static void main(String[] args)throws Throwable {
        check(Build.VERSION.SDK_INT==32 && args.length==4 && args[3].matches("[a-f0-9]{32}"),"Expected API32 original APK, patched APK, fresh directory and nonce");
        File root=new File(args[2]);check(root.mkdir(),"Use a fresh proof directory");
        JSONObject before=run(args[0],new File(root,"original").getPath(),false);
        JSONObject after=run(args[1],new File(root,"patched").getPath(),true);
        System.out.println("POCKET_LAUNCHER_RECENTS_NULL32 "+new JSONObject().put("validation","PASS").put("sdk",Build.VERSION.SDK_INT)
            .put("uid",Os.getuid()).put("pid",Os.getpid()).put("nonce",args[3]).put("original",before).put("patched",after)
            .put("scope","Actual Launcher DEX on ART with controlled interface results; real task UI and Binder permission enforcement require separate integration checks"));
    }
}
