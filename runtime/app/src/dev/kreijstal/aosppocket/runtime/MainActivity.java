// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.app.ActivityManager;
import android.content.*;
import android.graphics.Color;
import android.os.Bundle;
import android.widget.*;

/** Manager task with a live miniature; the guest screen has its own task/icon. */
public final class MainActivity extends RuntimeActivity {
    private Button install, start, stop, apk;
    private Spinner version;
    private boolean updating;
    private android.net.Uri pendingImport, pendingApk;
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        autoStart = getIntent().getBooleanExtra("start_guest", false);
        setTaskDescription(new ActivityManager.TaskDescription("Pocket Manager", null, 0xff24465e));
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(dp(12), dp(8), dp(12), dp(8));
        status = new TextView(this);
        status.setLines(2);
        layout.addView(status);
        version = new Spinner(this);
        version.setAdapter(new ArrayAdapter<String>(this, android.R.layout.simple_spinner_dropdown_item,
            new String[]{"Android 7.1.1 · PRoot", "Android 9 · direct backend"}));
        version.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> parent, android.view.View view, int position, long id) {
                if (!updating && runtime != null) runtime.selectSdk(position == 1 ? 28 : 25);
            }
            @Override public void onNothingSelected(AdapterView<?> parent) { }
        });
        layout.addView(version);
        LinearLayout controls = new LinearLayout(this);
        install = button("Import guest", controls, v -> importGuest());
        start = button("Start", controls, v -> requestStart());
        stop = button("Stop", controls, v -> { if (runtime != null) runtime.stopGuest(); });
        layout.addView(controls, new LinearLayout.LayoutParams(-1, dp(48)));
        TextView caption = new TextView(this);
        caption.setText("Android display");
        caption.setPadding(0, dp(12), 0, dp(8));
        layout.addView(caption);
        display = new GuestDisplay(this, false);
        display.limitPreviewHeight(dp(300));
        layout.addView(display, new LinearLayout.LayoutParams(-1, 0, 1));
        LinearLayout open = new LinearLayout(this);
        apk = button("Install APK", open, v -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
            .setType("application/vnd.android.package-archive").addCategory(Intent.CATEGORY_OPENABLE), 2));
        button("Open Android ↗", open, v -> startActivity(GuestActivity.intent(this)));
        button("Close manager", open, v -> finishAndRemoveTask());
        layout.addView(open, new LinearLayout.LayoutParams(-1, dp(52)));
        TextView note = new TextView(this);
        note.setText("Android has its own launcher icon and Recents window. Closing this manager keeps it running. Shared host UID/kernel.");
        note.setTextColor(Color.DKGRAY);
        layout.addView(note);
        setContentView(layout);
        onGuestChanged();
    }
    @Override public void onGuestChanged() {
        super.onGuestChanged();
        if (start == null) return;
        if (runtime != null && pendingImport != null) {
            android.net.Uri uri = pendingImport;
            pendingImport = null;
            runtime.importGuest(uri);
        }
        if (runtime != null && pendingApk != null) {
            android.net.Uri uri = pendingApk; pendingApk = null;
            runtime.installApk(uri);
        }
        boolean ready = runtime != null;
        updating = true;
        if (ready) version.setSelection(runtime.selectedSdk() == 28 ? 1 : 0, false);
        version.setEnabled(ready && !runtime.running() && !runtime.starting() && !runtime.preparing() && !runtime.stopping());
        updating = false;
        apk.setEnabled(ready && runtime.directBackend() && runtime.booted() && !runtime.stopping());
        install.setEnabled(ready && !runtime.directBackend() && !runtime.hasImage() && !runtime.starting() && !runtime.running());
        start.setEnabled(ready && runtime.canStart() && !runtime.running() && !runtime.starting() && !runtime.stopping());
        stop.setEnabled(ready && (runtime.running() || runtime.preparing()) && !runtime.stopping());
    }
    private void importGuest() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("application/zip");
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        startActivityForResult(intent, 1);
    }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        if (request == 2) {
            pendingApk = data.getData();
            onGuestChanged();
            return;
        }
        if (request != 1) return;
        pendingImport = data.getData();
        onGuestChanged();
    }
}
