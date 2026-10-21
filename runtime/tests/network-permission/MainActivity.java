// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketnetpermissiontest;
import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
public final class MainActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView view=new TextView(this);view.setText("Network permission install passed");setContentView(view);
        android.util.Log.i("PocketNetworkPermissionTest","POCKET_PERMISSION_APP_STARTED");
    }
}
