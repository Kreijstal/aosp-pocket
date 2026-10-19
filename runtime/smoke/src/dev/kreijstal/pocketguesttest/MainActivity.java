// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketguesttest;
import android.app.Activity;
import android.os.Bundle;
import android.view.View;
import android.widget.*;

/** End-to-end proof of guest APK installation, rendering and forwarded touch. */
public final class MainActivity extends Activity {
    private int taps;
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        if (state != null) taps = state.getInt("taps");
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        TextView title = new TextView(this);
        title.setText("AOSP Pocket guest test\nAndroid " + android.os.Build.VERSION.RELEASE +
            " · guest UID " + android.os.Process.myUid());
        title.setTextSize(20);
        title.setTextColor(0xff111111);
        title.setPadding(16, 24, 16, 24);
        layout.setBackgroundColor(0xffeeeeee);
        layout.addView(title);
        final Button button = new Button(this);
        button.setText("Guest taps: " + taps);
        button.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View view) {
                button.setText("Guest taps: " + (++taps));
                android.util.Log.i("PocketGuestTest", "tap=" + taps);
            }
        });
        layout.addView(button);
        for (int color : new int[]{0xffff0000, 0xff00ff00, 0xff0000ff}) {
            View swatch = new View(this);
            swatch.setBackgroundColor(color);
            layout.addView(swatch, new LinearLayout.LayoutParams(-1, 80));
        }
        setContentView(layout);
    }
    @Override public void onSaveInstanceState(Bundle out) { out.putInt("taps", taps); super.onSaveInstanceState(out); }
}
