// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketmicrogtest;

import android.app.Activity;
import android.content.ContentProviderClient;
import android.database.Cursor;
import android.os.Bundle;
import android.os.SystemClock;
import android.provider.ContactsContract;
import java.io.FileOutputStream;
import org.json.JSONArray;
import org.json.JSONObject;

/** Times normal Contacts access without logging contact values or changing deadlines. */
public final class ContactsTiming extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        new Thread(()->{
            JSONObject report=new JSONObject();JSONArray runs=new JSONArray();
            try {
                report.put("uid",android.os.Process.myUid());
                for(int i=0;i<3;i++) {
                    JSONObject run=new JSONObject();long start=SystemClock.elapsedRealtime();
                    android.util.Log.i("PocketContactsTiming","run="+i+" acquire start");
                    ContentProviderClient provider=getContentResolver().acquireUnstableContentProviderClient(ContactsContract.Contacts.CONTENT_URI);
                    if(provider==null)throw new IllegalStateException("No Contacts provider");
                    long acquired=SystemClock.elapsedRealtime();run.put("acquireMs",acquired-start);
                    android.util.Log.i("PocketContactsTiming","run="+i+" acquireMs="+(acquired-start)+" query start");
                    try {
                        Cursor cursor=provider.query(ContactsContract.Contacts.CONTENT_URI,new String[]{"_id"},null,null,null);
                        long queried=SystemClock.elapsedRealtime();run.put("queryMs",queried-acquired);
                        if(cursor==null)throw new IllegalStateException("Null cursor");
                        try {
                            run.put("rows",cursor.getCount());run.put("countMs",SystemClock.elapsedRealtime()-queried);
                        }finally{cursor.close();}
                    }finally{provider.close();}
                    run.put("totalMs",SystemClock.elapsedRealtime()-start);runs.put(run);
                    android.util.Log.i("PocketContactsTiming","run="+i+" "+run);
                }
                report.put("validation","PASS");
            }catch(Throwable error){try{report.put("validation","FAIL");report.put("error",String.valueOf(error));}catch(Exception ignored){}}
            try {
                report.put("runs",runs);
                try(FileOutputStream out=openFileOutput("contacts-timing.json",0)){out.write(report.toString(2).getBytes("UTF-8"));}
            }catch(Exception error){android.util.Log.e("PocketContactsTiming","report failed",error);}
        },"ContactsTiming").start();
    }
}
