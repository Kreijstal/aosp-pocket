// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketinstallfuture;
public final class FutureActivity extends android.app.Activity {
    @Override public void onCreate(android.os.Bundle state) {
        super.onCreate(state);
        android.widget.TextView text=new android.widget.TextView(this);
        text.setText("Android 10 minimum-version fixture");setContentView(text);
    }
}
