// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pockethostnetworktest;
import android.app.Activity;
import android.content.Intent;
import android.net.*;
import android.os.*;
import android.util.Log;
import android.widget.*;
import org.json.JSONObject;
import java.net.*;
import java.nio.file.*;

/** No private bridge SDK or system UID: exercise ordinary Android app APIs. */
public final class MainActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);LinearLayout layout=new LinearLayout(this);layout.setOrientation(LinearLayout.VERTICAL);
        TextView status=new TextView(this);status.setLines(8);status.setText("Checking the guest's normal network APIs…");layout.addView(status);
        Button sms=new Button(this);sms.setText("Open host SMS composer");layout.addView(sms);
        sms.setOnClickListener(v->startActivity(new Intent(Intent.ACTION_SENDTO,Uri.fromParts("smsto","",null))));
        sms.post(()->{int[] position=new int[2];sms.getLocationOnScreen(position);
            Log.i("PocketHostSmsTest","center="+(position[0]+sms.getWidth()/2)+","+(position[1]+sms.getHeight()/2));});
        setContentView(layout);
        new Thread(()->{
            JSONObject result=new JSONObject();
            try {
                result.put("guest_uid",android.os.Process.myUid());
                for(String line:Files.readAllLines(Paths.get("/proc/self/status")))
                    if(line.startsWith("Uid:"))result.put("kernel_uid",Integer.parseInt(line.split("\\s+")[1]));
                ConnectivityManager cm=(ConnectivityManager)getSystemService(CONNECTIVITY_SERVICE);
                result.put("connected",cm.getActiveNetworkInfo()!=null && cm.getActiveNetworkInfo().isConnected());
                result.put("dns_addresses",InetAddress.getAllByName("example.com").length);
                HttpURLConnection connection=(HttpURLConnection)new URL("https://example.com/").openConnection();
                connection.setConnectTimeout(10000);connection.setReadTimeout(10000);
                try {result.put("http_status",connection.getResponseCode());}finally{connection.disconnect();}
                result.put("passed",result.getInt("guest_uid")>=10000 && result.getBoolean("connected") && result.getInt("http_status")==200);
            } catch(Exception error){try{result.put("passed",false).put("error",error.toString());}catch(Exception ignored){}}
            Log.i("PocketHostNetworkTest",result.toString());
            runOnUiThread(()->status.setText(result.toString()));
        },"GuestNetworkTest").start();
    }
}
