// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketcodec32test;
import android.app.Activity;
import android.os.*;
import android.widget.TextView;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.JSONObject;

/** Ordinary app exercises the public MediaCodec API; no platform signature or permissions. */
public final class MainActivity extends Activity {
    static volatile String snapshot="{}";
    private static final AtomicBoolean running=new AtomicBoolean();
    private static int generation;
    private TextView text;
    @Override public void dump(String prefix,java.io.FileDescriptor fd,java.io.PrintWriter out,String[] args) {
        out.println("POCKET_CODEC_APP32="+android.util.Base64.encodeToString(snapshot.getBytes(java.nio.charset.StandardCharsets.UTF_8),android.util.Base64.NO_WRAP));
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);text=new TextView(this);text.setTextSize(20);text.setPadding(24,32,24,24);setContentView(text);
    }
    @Override public void onResume() {
        super.onResume();if(!running.compareAndSet(false,true))return;
        final JSONObject report=new JSONObject();
        try {
            if(Build.VERSION.SDK_INT!=32 || android.os.Process.myUid()<10000 || getApplicationInfo().uid!=android.os.Process.myUid())
                throw new IllegalStateException("An ordinary SDK32 app UID is required");
            report.put("sdk",Build.VERSION.SDK_INT).put("uid",android.os.Process.myUid()).put("pid",android.os.Process.myPid())
                .put("package",getPackageName()).put("nonce",UUID.randomUUID().toString()).put("generation",++generation)
                .put("startedUptimeMillis",SystemClock.uptimeMillis()).put("validation","RUNNING");
            snapshot=report.toString();text.setText("Running AAC and AVC codec round trips");
        }catch(Exception error){running.set(false);throw new IllegalStateException(error);}
        new Thread(()->{
            try {
                CodecRoundTrip.run(report,stage->{
                    report.put("stage",stage);snapshot=report.toString();
                });
                report.put("validation","PASS");
            }catch(Throwable error) {
                try {
                    report.put("validation","FAIL").put("error",error.toString());
                    if(error instanceof android.media.MediaCodec.CodecException) {
                        android.media.MediaCodec.CodecException codec=(android.media.MediaCodec.CodecException)error;
                        report.put("codecErrorCode",codec.getErrorCode()).put("codecDiagnostic",codec.getDiagnosticInfo())
                            .put("recoverable",codec.isRecoverable()).put("transient",codec.isTransient());
                    }
                }catch(Exception ignored){}
            }finally {
                try{report.put("finishedUptimeMillis",SystemClock.uptimeMillis());}catch(Exception ignored){}
                snapshot=report.toString();running.set(false);runOnUiThread(()->text.setText(snapshot));
            }
        },"PublicMediaCodec32").start();
    }
}
