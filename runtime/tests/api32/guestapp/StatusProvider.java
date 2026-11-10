// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketapi32guesttest;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import java.util.Map;

/** Read-only test feedback. Generic provider calls enforce their own caller. */
public final class StatusProvider extends ContentProvider {
    @Override public boolean onCreate(){return true;}
    @Override public Bundle call(String method,String arg,Bundle extras) {
        int uid=Binder.getCallingUid();
        if(uid!=0 && uid!=1000)throw new SecurityException("System test tool required");
        if("trace".equals(method)) {
            try{android.system.Os.kill(android.os.Process.myPid(),3);}
            catch(android.system.ErrnoException error){throw new IllegalStateException(error);}
            Bundle result=new Bundle();result.putInt("pid",android.os.Process.myPid());result.putInt("uid",android.os.Process.myUid());return result;
        }
        if(!"snapshot".equals(method))throw new IllegalArgumentException("Unknown method");
        Bundle result=new Bundle();
        SharedPreferences state=getContext().getSharedPreferences("state",0);
        for(Map.Entry<String,?> entry:state.getAll().entrySet()) {
            Object value=entry.getValue();String key=entry.getKey();
            if(key.equals("encrypted") || key.equals("iv"))continue;
            if(value instanceof String)result.putString(key,(String)value);
            else if(value instanceof Boolean)result.putBoolean(key,(Boolean)value);
            else if(value instanceof Integer)result.putInt(key,(Integer)value);
            else if(value instanceof Long)result.putLong(key,(Long)value);
        }
        result.putInt("providerPid",android.os.Process.myPid());result.putInt("providerUid",android.os.Process.myUid());
        return result;
    }
    @Override public Cursor query(Uri uri,String[] projection,String selection,String[] args,String order){throw new UnsupportedOperationException();}
    @Override public String getType(Uri uri){return "application/vnd.pocket.api32-test";}
    @Override public Uri insert(Uri uri,ContentValues values){throw new UnsupportedOperationException();}
    @Override public int delete(Uri uri,String selection,String[] args){throw new UnsupportedOperationException();}
    @Override public int update(Uri uri,ContentValues values,String selection,String[] args){throw new UnsupportedOperationException();}
}
