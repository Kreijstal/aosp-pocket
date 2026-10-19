// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.app.Activity;
import android.content.*;
import android.os.*;
import android.view.*;
import android.widget.*;

/** UI binding only. Destroying a window must never terminate the guest. */
abstract class RuntimeActivity extends Activity implements GuestService.Listener {
    GuestService runtime;
    GuestDisplay display;
    TextView status;
    boolean autoStart;
    private boolean bound;
    private final ServiceConnection connection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            runtime = ((GuestService.LocalBinder)binder).get();
            int sdk = getIntent().getIntExtra("guest_sdk", 0);
            if (sdk == 25 || sdk == 28) runtime.selectSdk(sdk);
            runtime.addListener(RuntimeActivity.this);
            if (autoStart) { autoStart = false; requestStart(); }
        }
        @Override public void onServiceDisconnected(ComponentName name) {
            if (display != null) display.setRuntime(null);
            runtime = null;
            onGuestChanged();
        }
    };
    @Override public void onCreate(Bundle state) {
        Native.configureHost();
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }
    @Override protected void onStart() {
        super.onStart();
        bound = bindService(new Intent(this, GuestService.class), connection, BIND_AUTO_CREATE);
    }
    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (intent.getBooleanExtra("start_guest", false)) requestStart();
    }
    @Override protected void onStop() {
        if (display != null) display.setRuntime(null);
        if (runtime != null) runtime.removeListener(this);
        if (bound) { unbindService(connection); bound = false; }
        runtime = null;
        super.onStop();
    }
    @Override public void onGuestChanged() {
        if (display != null) display.setRuntime(runtime);
        if (status != null) status.setText(runtime == null ? "Connecting to Android…" : runtime.status());
    }
    void requestStart() {
        if (runtime == null) { autoStart = true; return; }
        runtime.startGuest();
    }
    Button button(String text, LinearLayout parent, View.OnClickListener listener) {
        Button button = new Button(this);
        button.setText(text);
        button.setOnClickListener(listener);
        parent.addView(button, new LinearLayout.LayoutParams(0, -1, 1));
        return button;
    }
    int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    @Override public boolean dispatchTouchEvent(MotionEvent event) {
        if (getIntent().getBooleanExtra("trace_input", false) && (event.getActionMasked() == MotionEvent.ACTION_DOWN ||
                event.getActionMasked() == MotionEvent.ACTION_UP))
            android.util.Log.i("PocketRuntime", getClass().getSimpleName() + " touch action=" +
                event.getActionMasked() + " x=" + event.getX() + " y=" + event.getY());
        return super.dispatchTouchEvent(event);
    }
}
