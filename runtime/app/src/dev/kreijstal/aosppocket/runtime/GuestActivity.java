// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.app.*;
import android.content.*;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.os.Bundle;
import android.util.Rational;
import android.view.*;
import android.widget.*;

/** Dedicated Android task; PiP gives it a floating miniature without overlays. */
public final class GuestActivity extends RuntimeActivity {
    private LinearLayout controls;
    private Button floating, back, home;
    static Intent intent(Context context) {
        return new Intent(context, GuestActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        autoStart = true;
        setTaskDescription(new ActivityManager.TaskDescription("Pocket Android", null, 0xff16746e));
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        controls = new LinearLayout(this);
        button("Manager", controls, v -> startActivity(new Intent(this, MainActivity.class)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP)));
        floating = button("Float", controls, v -> enterFloating());
        back = button("Back", controls, v -> { if (runtime != null) runtime.key(KeyEvent.KEYCODE_BACK); });
        home = button("Home", controls, v -> { if (runtime != null) runtime.key(KeyEvent.KEYCODE_HOME); });
        button("Close window", controls, v -> finishAndRemoveTask());
        layout.addView(controls, new LinearLayout.LayoutParams(-1, dp(48)));
        FrameLayout content = new FrameLayout(this);
        display = new GuestDisplay(this, true);
        content.addView(display, new FrameLayout.LayoutParams(-1, -1));
        status = new TextView(this);
        status.setTextColor(0xffffffff);
        status.setBackgroundColor(0xaa000000);
        content.addView(status, new FrameLayout.LayoutParams(-1, -2));
        layout.addView(content, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(layout);
        immersive();
        onGuestChanged();
    }
    private void immersive() {
        if (!isInPictureInPictureMode()) getWindow().getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN |
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }
    @Override protected void onStart() { autoStart = true; super.onStart(); }
    @Override public void onWindowFocusChanged(boolean focus) { super.onWindowFocusChanged(focus); if (focus) immersive(); }
    @Override public void onGuestChanged() {
        super.onGuestChanged();
        if (back != null) {
            boolean enabled = runtime != null && runtime.directBackend() && runtime.booted();
            back.setEnabled(enabled); home.setEnabled(enabled);
        }
        if (floating != null) floating.setEnabled(runtime != null && runtime.running() &&
            getPackageManager().hasSystemFeature(PackageManager.FEATURE_PICTURE_IN_PICTURE));
        if (status != null) status.setVisibility(isInPictureInPictureMode() || runtime != null && runtime.booted() ? View.GONE : View.VISIBLE);
    }
    private void enterFloating() {
        if (runtime == null || !runtime.running()) return;
        PictureInPictureParams.Builder params = new PictureInPictureParams.Builder()
            .setAspectRatio(new Rational(runtime.width, runtime.height))
            .setSourceRectHint(display.contentBounds());
        if (android.os.Build.VERSION.SDK_INT >= 31) params.setSeamlessResizeEnabled(false);
        if (!enterPictureInPictureMode(params.build()))
            Toast.makeText(this, "Floating window is unavailable on this host", Toast.LENGTH_SHORT).show();
    }
    @Override public void onPictureInPictureModeChanged(boolean pip, Configuration configuration) {
        super.onPictureInPictureModeChanged(pip, configuration);
        android.util.Log.i("PocketWindows", "Guest PiP=" + pip + " task=" + getTaskId());
        controls.setVisibility(pip ? View.GONE : View.VISIBLE);
        onGuestChanged();
        if (!pip) immersive();
    }
}
