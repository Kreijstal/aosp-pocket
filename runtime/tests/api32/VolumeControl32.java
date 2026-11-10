// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.os.*;
import android.system.Os;
import java.io.IOException;
import java.lang.reflect.*;
import org.json.*;

/** Test-only owner entry through the actual framework, never directly through vold. */
public final class VolumeControl32 {
    private static final String ID="emulated;0";
    private static void check(boolean value,String why)throws IOException {if(!value)throw new IOException(why);}
    private static final class ResetEvents implements AutoCloseable {
        final Class<?> api,listenerType=Class.forName("android.os.storage.IStorageEventListener");
        final Object manager,listener;
        final java.util.List<JSONObject> events=new java.util.ArrayList<>();
        Throwable failure;
        ResetEvents(Class<?> api,Object manager)throws Exception {
            this.api=api;this.manager=manager;
            Class<?> stub=Class.forName("android.os.storage.IStorageEventListener$Stub");
            Field code=stub.getDeclaredField("TRANSACTION_onVolumeStateChanged");code.setAccessible(true);
            final int changed=code.getInt(null);
            final Parcelable.Creator<?> creator=(Parcelable.Creator<?>)Class.forName("android.os.storage.VolumeInfo").getField("CREATOR").get(null);
            Binder receiver=new Binder(){
                @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException {
                    if(code==INTERFACE_TRANSACTION){reply.writeString("android.os.storage.IStorageEventListener");return true;}
                    if(code<IBinder.FIRST_CALL_TRANSACTION || code>IBinder.LAST_CALL_TRANSACTION)return super.onTransact(code,data,reply,flags);
                    synchronized(ResetEvents.this) {
                        try {
                            data.enforceInterface("android.os.storage.IStorageEventListener");
                            if(code==changed) {
                                Object item=data.readInt()!=0?creator.createFromParcel(data):null;
                                int oldState=data.readInt(),newState=data.readInt();
                                check(item!=null,"Missing framework volume callback");
                                Class<?> type=item.getClass();String id=(String)type.getMethod("getId").invoke(item);
                                if(ID.equals(id)) {
                                    check(events.size()<128,"Too many framework volume callbacks");
                                    events.add(new JSONObject().put("id",id).put("type",type.getMethod("getType").invoke(item))
                                        .put("user",type.getMethod("getMountUserId").invoke(item)).put("oldState",oldState)
                                        .put("newState",newState).put("elapsedMillis",SystemClock.elapsedRealtime()));
                                }
                            }
                        }catch(Throwable error){failure=error;}
                        ResetEvents.this.notifyAll();
                    }
                    return true;
                }
            };
            receiver.attachInterface(null,"android.os.storage.IStorageEventListener");
            listener=stub.getMethod("asInterface",IBinder.class).invoke(null,receiver);
            api.getMethod("registerListener",listenerType).invoke(manager,listener);
        }
        synchronized boolean remounted(long began)throws Exception {
            if(failure!=null)throw new IOException("Framework volume callback failed",failure);
            int phase=0;
            for(JSONObject event:events) {
                if(event.getLong("elapsedMillis")<began)continue;
                check(event.getInt("type")==2 && event.getInt("user")==0,"Unexpected reset volume identity");
                int oldState=event.getInt("oldState"),state=event.getInt("newState");
                // Stock H_RESET clears old queued state-change messages in
                // StorageSessionController.onReset. The recreated volume
                // still emits CHECKING then MOUNTED. The caller separately
                // verifies native reset, epoch change and holder retirement.
                if(phase==0 && oldState==0 && state==1)phase=1;
                else if(phase==1 && oldState==1 && state==2)return true;
            }
            return false;
        }
        synchronized JSONArray snapshot(){return new JSONArray(events);}
        @Override public void close()throws Exception{api.getMethod("unregisterListener",listenerType).invoke(manager,listener);}
    }
    private static JSONObject reset(Class<?> api,Object manager)throws Exception {
        Class<?> properties=Class.forName("android.os.SystemProperties"),storage=Class.forName("android.os.storage.StorageManager");
        Method get=properties.getMethod("get",String.class,String.class);String name="persist.sys.adoptable";
        String before=(String)get.invoke(null,name,"");
        int on=storage.getField("DEBUG_ADOPTABLE_FORCE_ON").getInt(null),off=storage.getField("DEBUG_ADOPTABLE_FORCE_OFF").getInt(null);
        check(before.isEmpty() || before.equals("force_on") || before.equals("force_off"),"Unexpected adoptable storage property");
        // Reapply the identical guest value. Stock StorageManagerService posts
        // H_RESET, which recreates volume sessions without resetting user data.
        try(ResetEvents events=new ResetEvents(api,manager)) {
            long began=SystemClock.elapsedRealtime();
            api.getMethod("setDebugFlags",int.class,int.class).invoke(manager,before.equals("force_on")?on:before.equals("force_off")?off:0,on|off);
            long deadline=began+60000;
            while(!events.remounted(began)) {
                check(SystemClock.elapsedRealtime()<deadline,"Framework reset did not emit recreated checking/mounted callbacks: "+events.snapshot());
                Thread.sleep(50);
            }
            String after=(String)get.invoke(null,name,"");check(before.equals(after),"Reset changed the guest storage property");
            return new JSONObject().put("requestElapsedMillis",began).put("events",events.snapshot())
                .put("property",name).put("propertyBefore",before).put("propertyAfter",after)
                .put("debugFlags",before.equals("force_on")?on:before.equals("force_off")?off:0).put("debugMask",on|off)
                .put("via","IStorageManager.setDebugFlags");
        }
    }
    private static JSONObject volume(Class<?> api,Object manager)throws Exception {
        Object[] volumes=(Object[])api.getMethod("getVolumes",int.class).invoke(manager,0);JSONObject result=null;
        for(Object v:volumes) {
            Class<?> type=v.getClass();if(!ID.equals(type.getMethod("getId").invoke(v)))continue;
            check(result==null,"Duplicate emulated volume");
            result=new JSONObject().put("id",ID).put("type",type.getMethod("getType").invoke(v))
                .put("state",type.getMethod("getState").invoke(v)).put("user",type.getMethod("getMountUserId").invoke(v));
        }
        check(result!=null && result.getInt("type")==2 && result.getInt("user")==0,"Expected user0 emulated volume");return result;
    }
    public static void main(String[] args) {
        try {
            check(Build.VERSION.SDK_INT==32 && Os.getuid()==0 && args.length==2
                && (args[0].equals("mount") || args[0].equals("unmount") || args[0].equals("state") || args[0].equals("reset")) && args[1].matches("[a-f0-9]{32}"),"Invalid SDK32 owner request");
            Os.setgid(1000);Os.setuid(1000);Looper.prepare();
            Class<?> api=Class.forName("android.os.storage.IStorageManager");
            IBinder binder=(IBinder)Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,"mount");
            Object manager=Class.forName("android.os.storage.IStorageManager$Stub").getMethod("asInterface",IBinder.class).invoke(null,binder);
            JSONObject before=volume(api,manager);boolean resetting=args[0].equals("reset");int expected=args[0].equals("mount") || resetting?2:0;
            boolean observe=args[0].equals("state");
            if(resetting)check(before.getInt("state")==2,"Reset requires a mounted volume");
            if(!observe && !resetting)check(before.getInt("state")== (expected==2?0:2),"Unexpected initial volume state: "+before);
            long began=SystemClock.elapsedRealtime();JSONObject resetProof=resetting?reset(api,manager):null;
            if(!observe && !resetting)api.getMethod(args[0],String.class).invoke(manager,ID);
            long deadline=SystemClock.elapsedRealtime()+60000;JSONObject after;
            while(true) {
                after=volume(api,manager);if(observe || after.getInt("state")==expected)break;
                check(SystemClock.elapsedRealtime()<deadline,"Framework volume did not reach expected state: "+after);Thread.sleep(100);
            }
            System.out.println("POCKET_VOLUME_CONTROL32 "+new JSONObject().put("sdk",32).put("uid",Os.getuid())
                .put("nonce",args[1]).put("operation",args[0]).put("via","IStorageManager")
                .put("before",before).put("after",after).put("reset",resetProof).put("durationMillis",SystemClock.elapsedRealtime()-began).put("validation","PASS"));
            System.out.flush();System.exit(0);
        }catch(Throwable error) {
            while(error instanceof InvocationTargetException && error.getCause()!=null)error=error.getCause();
            System.err.println("POCKET_VOLUME_CONTROL32_ERROR "+error);error.printStackTrace();System.exit(1);
        }
    }
}
