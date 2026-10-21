// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketsyscalltest;
import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.widget.TextView;
import java.io.FileOutputStream;
import org.json.JSONObject;

public class MainActivity extends Activity {
    static {System.loadLibrary("syscall_fixture");}
    private static native String probe(String path);
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        TextView text=new TextView(this);text.setText("Testing selected syscall profile…");setContentView(text);
        final boolean baseline=this instanceof BaselineActivity;
        new Thread(()->{
            try {
                JSONObject result=new JSONObject(probe("/data/local/tmp/pocket-syscall-fixture/fixture.txt"));
                result.put("profile",baseline?"baseline":"adapted").put("guestUid",android.os.Process.myUid());
                String json=result.toString();
                try(FileOutputStream out=openFileOutput(baseline?"baseline.json":"adapted.json",MODE_PRIVATE)) {
                    out.write(json.getBytes("UTF-8"));
                }
                android.util.Log.i("PocketSyscallTest",json);
                runOnUiThread(()->{
                    text.setText(json);
                    if(!baseline)startActivity(new Intent(this,BaselineActivity.class));
                });
            } catch(Throwable error) {android.util.Log.e("PocketSyscallTest","probe failed",error);}
        },"SyscallProfileTest").start();
    }
}
