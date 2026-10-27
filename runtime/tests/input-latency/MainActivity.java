// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketinputlatency;

import android.app.Activity;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.WindowManager;
import android.widget.TextView;
import java.io.FileOutputStream;

/** A real application deliberately spending 1.2 seconds inside a touch callback. */
public final class MainActivity extends Activity {
    private boolean down;
    private void write(String name,String value) {
        try(FileOutputStream out=openFileOutput(name,MODE_PRIVATE)) {out.write(value.getBytes("UTF-8"));}
        catch(Exception failure){throw new RuntimeException(failure);}
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON|WindowManager.LayoutParams.FLAG_FULLSCREEN);
        TextView view=new TextView(this);
        view.setText("Pocket input latency test\nThe test briefly stalls this app's touch handler.");
        view.setTextSize(20);view.setGravity(android.view.Gravity.CENTER);setContentView(view);
        view.setOnTouchListener((target,event)->{
            if(event.getActionMasked()==MotionEvent.ACTION_DOWN) {
                down=true;
                try {Thread.sleep(1200);}catch(InterruptedException interrupted){Thread.currentThread().interrupt();}
            } else if(event.getActionMasked()==MotionEvent.ACTION_UP) {
                write("result.json","{\"orderedDownUp\":"+down+",\"handlerDelayMs\":1200}");down=false;
            }
            return true;
        });
    }
    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if(focused)write("ready.json","{\"ready\":true}");
    }
}
