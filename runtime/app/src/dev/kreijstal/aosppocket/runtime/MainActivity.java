// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.app.ActivityManager;
import android.content.*;
import android.graphics.Color;
import android.os.Bundle;
import android.widget.*;

/** Manager task with a live miniature; the guest screen has its own task/icon. */
public final class MainActivity extends RuntimeActivity {
    private Button install, start, stop;
    private android.net.Uri pendingImport;
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
        LinearLayout controls = new LinearLayout(this);
        install = button("Import guest", controls, v -> importGuest());
        start = button("Start", controls, v -> requestStart());
        stop = button("Stop", controls, v -> { if (runtime != null) runtime.stopGuest(); });
        layout.addView(controls, new LinearLayout.LayoutParams(-1, dp(48)));
        TextView caption = new TextView(this);
        caption.setText("Live Android preview");
        caption.setPadding(0, dp(12), 0, dp(8));
        layout.addView(caption);
        display = new GuestDisplay(this);
        display.limitPreviewHeight(dp(300));
        layout.addView(display, new LinearLayout.LayoutParams(-1, 0, 1));
        LinearLayout open = new LinearLayout(this);
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
        boolean ready = runtime != null;
        install.setEnabled(ready && !runtime.hasImage() && !runtime.starting() && !runtime.running());
        start.setEnabled(ready && runtime.canStart() && !runtime.running() && !runtime.starting());
        stop.setEnabled(ready && (runtime.running() || runtime.preparing()) && !runtime.stopping());
    }
    private void importGuest() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("application/zip");
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        startActivityForResult(intent, 1);
    }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != 1 || result != RESULT_OK || data == null || data.getData() == null) return;
        pendingImport = data.getData();
        onGuestChanged();
    }
}
