// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guestbridge;

import android.content.*;
import android.content.pm.PackageInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.*;
import android.os.Process;

/** A real AMS application observes package changes for Pocket's system tools. */
public final class PackageEventsProvider extends ContentProvider {
    private final Object lock=new Object();
    private SharedPreferences state;
    private final BroadcastReceiver receiver=new BroadcastReceiver(){
        @Override public void onReceive(Context context,Intent intent) {
            Uri data=intent.getData();
            if(data==null || !"package".equals(data.getScheme()))return;
            String pkg=data.getSchemeSpecificPart();
            synchronized(lock) {
                if(!pkg.equals(state.getString("target",null)))return;
                SharedPreferences.Editor edit=state.edit().putLong("sequence",state.getLong("sequence",0)+1)
                    .putString("eventPackage",pkg).putString("eventAction",intent.getAction())
                    .putLong("updateTime",-1).putInt("eventUid",-1).remove("error");
                try {
                    PackageInfo info=context.getPackageManager().getPackageInfo(pkg,0);
                    edit.putLong("updateTime",info.lastUpdateTime).putLong("versionCode",info.getLongVersionCode())
                        .putInt("eventUid",info.applicationInfo.uid);
                }catch(Exception error){edit.putString("error",error.toString());}
                if(!edit.commit())throw new IllegalStateException("Cannot persist package event");
            }
        }
    };
    @Override public boolean onCreate() {
        if(Build.VERSION.SDK_INT!=32 || Process.myUid()<10000)throw new IllegalStateException("Expected an installed SDK 32 application");
        state=getContext().getSharedPreferences("package-events",Context.MODE_PRIVATE);
        IntentFilter filter=new IntentFilter();filter.addAction(Intent.ACTION_PACKAGE_ADDED);
        filter.addAction(Intent.ACTION_PACKAGE_REPLACED);filter.addDataScheme("package");
        getContext().registerReceiver(receiver,filter);
        return true;
    }
    @Override public Bundle call(String method,String arg,Bundle extras) {
        int uid=Binder.getCallingUid();
        if(uid!=0 && uid!=1000)throw new SecurityException("Pocket system tool required");
        if("dns".equals(method)) {
            long identity=Binder.clearCallingIdentity();
            try{return DnsApi32.check(getContext());}finally{Binder.restoreCallingIdentity(identity);}
        }
        if("trace".equals(method)) {
            try{android.system.Os.kill(Process.myPid(),3);}
            catch(android.system.ErrnoException error){throw new IllegalStateException(error);}
        } else if(!"prepare".equals(method) && !"snapshot".equals(method))throw new IllegalArgumentException("Unknown method");
        synchronized(lock) {
            if("prepare".equals(method)) {
                if(arg==null || !arg.matches("[A-Za-z0-9_]+(\\.[A-Za-z0-9_]+)+"))throw new IllegalArgumentException("Invalid package name");
                if(!state.edit().putString("target",arg).remove("eventPackage").remove("eventAction")
                    .putLong("updateTime",-1).remove("error").commit())throw new IllegalStateException("Cannot prepare package observer");
            }
            Bundle result=new Bundle();result.putString("target",state.getString("target",null));
            result.putLong("sequence",state.getLong("sequence",0));
            result.putString("eventPackage",state.getString("eventPackage",null));
            result.putString("eventAction",state.getString("eventAction",null));
            result.putLong("updateTime",state.getLong("updateTime",-1));
            result.putLong("versionCode",state.getLong("versionCode",-1));
            result.putInt("eventUid",state.getInt("eventUid",-1));
            result.putString("error",state.getString("error",null));
            result.putInt("pid",Process.myPid());result.putInt("uid",Process.myUid());
            return result;
        }
    }
    @Override public Cursor query(Uri uri,String[] projection,String selection,String[] args,String order){throw new UnsupportedOperationException();}
    @Override public String getType(Uri uri){return "application/vnd.pocket.package-events";}
    @Override public Uri insert(Uri uri,ContentValues values){throw new UnsupportedOperationException();}
    @Override public int delete(Uri uri,String selection,String[] args){throw new UnsupportedOperationException();}
    @Override public int update(Uri uri,ContentValues values,String selection,String[] args){throw new UnsupportedOperationException();}
}
