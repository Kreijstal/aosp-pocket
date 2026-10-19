// SPDX-License-Identifier: Apache-2.0
package dev.kreijstal.aosppocket;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Color;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.widget.*;
import java.io.File;
import java.io.FileInputStream;
import org.json.JSONObject;

public final class MainActivity extends Activity {
    private TextView status, output;
    private Button run;
    private volatile boolean running;
    private String latest = "";

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        int padding = (int)(20 * getResources().getDisplayMetrics().density);
        page.setPadding(padding, padding, padding, padding);
        page.setBackgroundColor(Color.rgb(18, 25, 33));
        TextView title = text("AOSP Pocket", 26);
        page.addView(title);
        page.addView(text("Native bootstrap · 0.1\nA source-built foundation for an Android-on-Android container.", 15));
        page.addView(text("This build runs AOSP command-line tools. A guest Android framework and desktop are not implemented yet.", 14));
        status = text("Ready. No root or wireless-debugging pairing required.", 15);
        page.addView(status);
        run = new Button(this);
        run.setText("Run AOSP bootstrap");
        run.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View view) { startBootstrap(); }
        });
        page.addView(run);
        Button copy = new Button(this);
        copy.setText("Copy report");
        copy.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View view) {
                ((ClipboardManager)getSystemService(Context.CLIPBOARD_SERVICE))
                        .setPrimaryClip(ClipData.newPlainText("AOSP Pocket report", latest));
                Toast.makeText(MainActivity.this, "Report copied", Toast.LENGTH_SHORT).show();
            }
        });
        page.addView(copy);
        ScrollView scroll = new ScrollView(this);
        output = text("The report records real child-process results and runtime limits.", 12);
        output.setTextIsSelectable(true);
        output.setTypeface(android.graphics.Typeface.MONOSPACE);
        scroll.addView(output);
        page.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(page);
        File existing = new File(getFilesDir(), Bootstrap.REPORT);
        if (existing.isFile()) {
            try { latest = Bootstrap.read(new FileInputStream(existing), 131072); output.setText(latest); }
            catch (Exception e) { output.setText(e.toString()); }
        }
        if (getIntent().getBooleanExtra("run_bootstrap", false)) startBootstrap();
    }

    private TextView text(String value, int sp) {
        TextView label = new TextView(this);
        label.setText(value);
        label.setTextSize(sp);
        label.setTextColor(Color.rgb(226, 235, 244));
        label.setPadding(0, 8, 0, 8);
        return label;
    }

    private void startBootstrap() {
        if (running) return;
        running = true;
        run.setEnabled(false);
        status.setText("Running bundled AOSP tools as the app's own UID…");
        new Thread(new Runnable() {
            @Override public void run() {
                String result;
                boolean success = false;
                try {
                    JSONObject report = Bootstrap.run(getApplicationContext());
                    success = report.getBoolean("bootstrap_ok");
                    result = report.toString(2);
                } catch (Exception e) {
                    result = "Bootstrap failed: " + e;
                    Log.e("AospPocket", "Bootstrap failed", e);
                }
                final String display = result;
                final boolean okay = success;
                Log.i("AospPocket", "native_bootstrap=" + okay);
                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        latest = display;
                        output.setText(display);
                        status.setText(okay ? "Native bootstrap passed. Guest Android is not booted yet."
                                : "Native bootstrap failed; see the actual errors below.");
                        run.setEnabled(true);
                        running = false;
                    }
                });
            }
        }, "pocket-bootstrap").start();
    }
}
