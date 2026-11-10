// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.content.*;
import android.content.pm.PackageInfo;
import android.os.*;
import java.io.IOException;

/** Holds a real AMS receiver until the installed package's broadcasts finish. */
final class PackageEvents32 implements AutoCloseable {
    private static final String AUTHORITY="dev.kreijstal.aosppocket.guestbridge.events";
    private final Class<?> api=Class.forName("android.app.IActivityManager");
    private final Object manager=Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
    private final IBinder token=new Binder();
    private final ContentProviderClient client;
    private final java.util.function.Consumer<String> log;
    PackageEvents32(Context context)throws Exception {
        this(context,System.out::println);
    }
    PackageEvents32(Context context,java.util.function.Consumer<String> log)throws Exception {
        this.log=log;
        Object holder=api.getMethod("getContentProviderExternal",String.class,int.class,IBinder.class,String.class)
            .invoke(manager,AUTHORITY,0,token,"pocket-manager-install");
        if(holder==null)throw new IOException("Package event provider is unavailable");
        try {
            Object provider=holder.getClass().getField("provider").get(holder);
            client=(ContentProviderClient)ContentProviderClient.class.getConstructor(ContentResolver.class,
                Class.forName("android.content.IContentProvider"),String.class,boolean.class)
                .newInstance(context.getContentResolver(),provider,AUTHORITY,true);
        }catch(Exception error){try{release();}catch(Exception cleanup){error.addSuppressed(cleanup);}throw error;}
    }
    long prepare(String name)throws Exception {
        Bundle state=client.call("prepare",name,null);
        if(state==null || state.getInt("uid")<10000)throw new IOException("Package receiver is not an ordinary AMS app");
        return state.getLong("sequence");
    }
    String dns()throws Exception {
        Bundle state=client.call("dns",null,null);
        if(state==null || state.getString("error")!=null || state.getString("result")==null)
            throw new IOException("Ordinary application DNS check failed: "+(state==null?"no result":state.getString("error")));
        return state.getString("result");
    }
    void await(PackageInfo installed,long sequence,boolean replacing,long deadline)throws Exception {
        String expected=replacing?Intent.ACTION_PACKAGE_REPLACED:Intent.ACTION_PACKAGE_ADDED;
        while(SystemClock.elapsedRealtime()<deadline) {
            Bundle state=client.call("snapshot",null,null);
            if(state!=null && state.getLong("sequence")>sequence && installed.packageName.equals(state.getString("eventPackage"))
                    && expected.equals(state.getString("eventAction"))) {
                if(state.getString("error")!=null || state.getLong("updateTime",-1)!=installed.lastUpdateTime
                    || state.getLong("versionCode",-1)!=installed.getLongVersionCode()
                    || state.getInt("eventUid",-1)!=installed.applicationInfo.uid || state.getInt("uid")<10000)
                    throw new IOException("Package event metadata does not match committed install");
                log.accept("POCKET_MANAGER32_INSTALL_READY action="+expected+" observer_uid="+state.getInt("uid")
                    +" package_uid="+installed.applicationInfo.uid+" sequence_delta="+(state.getLong("sequence")-sequence));
                return;
            }
            Thread.sleep(200);
        }
        throw new IOException("Package install broadcast did not finish before launch");
    }
    private void release()throws Exception {
        api.getMethod("removeContentProviderExternalAsUser",String.class,IBinder.class,int.class).invoke(manager,AUTHORITY,token,0);
    }
    public void close()throws Exception {try{client.close();}finally{release();}}
}
