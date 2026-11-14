// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ResolveInfo;
import android.os.Build;
import android.os.SystemClock;
import android.system.Os;
import android.util.SparseIntArray;
import java.io.IOException;
import java.lang.reflect.Array;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.List;
import org.json.JSONArray;
import org.json.JSONObject;

/** Read-only metadata from the pinned 12L AMS queues; never inspect Intent extras. */
final class BootBroadcasts32 {
    private static Object field(Object object,String name)throws Exception {
        Field field=object.getClass().getDeclaredField(name);field.setAccessible(true);return field.get(object);
    }
    private static boolean boot(Object record)throws Exception {
        String action=((Intent)field(record,"intent")).getAction();
        return Intent.ACTION_BOOT_COMPLETED.equals(action) || Intent.ACTION_LOCKED_BOOT_COMPLETED.equals(action);
    }
    private static String bounded(String value)throws IOException {
        if(value==null)return null;
        if(value.length()>300)throw new IOException("Broadcast metadata exceeds bound");return value;
    }
    private static Object receiver(List<?> receivers,int index)throws Exception {
        if(receivers==null || index<0 || index>=receivers.size())return JSONObject.NULL;
        Object receiver=receivers.get(index);
        if(receiver instanceof ResolveInfo) {
            ActivityInfo info=((ResolveInfo)receiver).activityInfo;
            if(info!=null)return bounded(new ComponentName(info.packageName,info.name).flattenToShortString());
        }
        return receiver==null?JSONObject.NULL:bounded(receiver.getClass().getName());
    }
    private static JSONObject record(Object record,String position,Object pending,long now)throws Exception {
        if(!record.getClass().getName().equals("com.android.server.am.BroadcastRecord"))
            throw new IOException("Unexpected Android12L broadcast record ABI");
        List<?> receivers=(List<?>)field(record,"receivers");
        int next=(Integer)field(record,"nextReceiver");
        int[] delivery=(int[])field(record,"delivery");
        if(delivery.length>10000 || (receivers!=null && receivers.size()>10000))throw new IOException("Too many broadcast receivers");
        int[] counts=new int[4];
        for(int state:delivery){if(state<0 || state>=counts.length)throw new IOException("Unexpected delivery state");counts[state]++;}
        ComponentName component=(ComponentName)field(record,"curComponent");
        Object app=field(record,"curApp");int pid=0,uid=-1;
        if(app!=null) {
            Method getPid=app.getClass().getDeclaredMethod("getPid");getPid.setAccessible(true);
            pid=(Integer)getPid.invoke(app);uid=(Integer)field(app,"uid");
        }
        long receiverTime=(Long)field(record,"receiverTime"),dispatchTime=(Long)field(record,"dispatchTime");
        JSONObject result=new JSONObject().put("position",position).put("id",System.identityHashCode(record))
            .put("pendingProcessStart",record==pending).put("action",bounded(((Intent)field(record,"intent")).getAction()))
            .put("userId",field(record,"userId")).put("state",field(record,"state"))
            .put("nextReceiver",next).put("receiverCount",receivers==null?0:receivers.size()).put("deliverySlots",delivery.length)
            .put("currentReceiver",component==null?JSONObject.NULL:bounded(component.flattenToShortString()))
            .put("previousReceiver",receiver(receivers,next-1)).put("nextReceiverComponent",receiver(receivers,next))
            .put("currentPid",pid).put("currentUid",uid).put("dispatchTime",dispatchTime).put("receiverTime",receiverTime)
            .put("receiverAgeMillis",receiverTime==0?0:Math.max(0,now-receiverTime))
            .put("timeoutExempt",field(record,"timeoutExempt")).put("deferred",field(record,"deferred"))
            .put("splitToken",field(record,"splitToken")).put("splitCount",field(record,"splitCount"))
            .put("hasResultCallback",field(record,"resultTo")!=null).put("anrCount",field(record,"anrCount"));
        return result.put("deliveryPending",counts[0]).put("deliveryDelivered",counts[1])
            .put("deliverySkipped",counts[2]).put("deliveryTimeout",counts[3]);
    }
    private static void queued(JSONArray records,List<?> list,String position,Object active,Object pending,long now)throws Exception {
        int count=Math.min(256,list.size());
        for(int i=0;i<count && records.length()<3;i++) {
            Object value=list.get(i);
            if(value!=active && value!=pending && boot(value))records.put(record(value,position+":"+i,pending,now));
        }
    }
    static JSONObject snapshot(Object service,String nonce)throws Exception {
        if(Build.VERSION.SDK_INT!=32 || Os.getuid()!=1000 || !nonce.matches("[0-9a-f]{32}")
                || !service.getClass().getName().equals("com.android.server.am.ActivityManagerService"))
            throw new IOException("Expected Android12L system-server broadcast observer");
        long now=SystemClock.uptimeMillis();JSONArray queues=new JSONArray();
        // BroadcastDispatcher uses the AMS instance as its lock. No Binder,
        // filesystem operations or callbacks occur while taking this snapshot.
        synchronized(service) {
            Object array=field(service,"mBroadcastQueues");
            if(Array.getLength(array)!=3 || !array.getClass().getComponentType().getName()
                    .equals("com.android.server.am.BroadcastQueue"))throw new IOException("Unexpected Android12L queue ABI");
            for(int i=0;i<3;i++) {
                Object queue=Array.get(array,i),dispatcher=field(queue,"mDispatcher");
                Object active=field(dispatcher,"mCurrentBroadcast"),pending=field(queue,"mPendingBroadcast");
                List<?> ordered=(List<?>)field(dispatcher,"mOrderedBroadcasts");
                List<?> deferred=(List<?>)field(dispatcher,"mDeferredBroadcasts"),alarms=(List<?>)field(dispatcher,"mAlarmBroadcasts");
                JSONArray records=new JSONArray();
                if(active!=null)records.put(record(active,"active",pending,now));
                if(pending!=null && pending!=active)records.put(record(pending,"pending",pending,now));
                queued(records,ordered,"ordered",active,pending,now);
                for(List<?> list:new List<?>[]{deferred,alarms}) {
                    for(int j=0;j<Math.min(256,list.size()) && records.length()<3;j++) {
                        Object entry=list.get(j);int before=records.length();
                        queued(records,(List<?>)field(entry,"broadcasts"),list==deferred?"deferred":"alarm",active,pending,now);
                        for(int k=before;k<records.length();k++)records.getJSONObject(k)
                            .put("deferredUid",field(entry,"uid")).put("deferUntil",field(entry,"deferUntil"));
                    }
                }
                SparseIntArray refs=(SparseIntArray)field(queue,"mSplitRefcounts");
                for(int j=0;j<records.length();j++) {
                    JSONObject item=records.getJSONObject(j);int token=item.getInt("splitToken");
                    item.put("callbackRefcount",token==0?0:refs.get(token,0));
                }
                queues.put(new JSONObject().put("name",bounded((String)field(queue,"mQueueName")))
                    .put("broadcastsScheduled",field(queue,"mBroadcastsScheduled"))
                    .put("timeoutMessagePending",field(queue,"mPendingBroadcastTimeoutMessage"))
                    .put("delayBehindServices",field(queue,"mDelayBehindServices"))
                    .put("timeoutMillis",field(field(queue,"mConstants"),"TIMEOUT"))
                    .put("orderedCount",ordered.size()).put("deferredUidCount",deferred.size())
                    .put("alarmUidCount",alarms.size()).put("recordLimit",3).put("records",records));
            }
        }
        JSONObject result=new JSONObject().put("schema",1).put("sdk",32).put("uid",Os.getuid()).put("pid",Os.getpid())
            .put("nonce",nonce).put("uptimeMillis",now).put("source","LocalServices.ActivityManagerService.mBroadcastQueues")
            .put("metadataOnly",true).put("queues",queues);
        if(result.toString().length()>12000)throw new IOException("Broadcast snapshot exceeds response bound");
        return result;
    }
}
