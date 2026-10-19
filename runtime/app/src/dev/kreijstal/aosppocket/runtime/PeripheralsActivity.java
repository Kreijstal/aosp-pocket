// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.Manifest;
import android.os.Bundle;
import android.widget.*;

/** Host grants remain explicit; a guest permission cannot grant host access. */
public final class PeripheralsActivity extends RuntimeActivity {
    private TextView summary;
    private Button contacts,network,devices,dialer;
    private boolean reading;
    private boolean pendingContacts;
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        ScrollView scroll=new ScrollView(this);LinearLayout layout=new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);layout.setPadding(dp(12),dp(8),dp(12),dp(8));scroll.addView(layout);
        TextView note=new TextView(this);
        note.setText("The guest can use host data only after Android grants this app access. Camera and microphone open only for a request and close afterwards. Standard guest HAL adapters are still being connected.");
        layout.addView(note);
        status=new TextView(this);layout.addView(status);
        grant(layout,"Camera",Manifest.permission.CAMERA);
        grant(layout,"Microphone",Manifest.permission.RECORD_AUDIO);
        grant(layout,"Contacts",Manifest.permission.READ_CONTACTS);
        grant(layout,"Phone state",Manifest.permission.READ_PHONE_STATE);
        grant(layout,"Location",Manifest.permission.ACCESS_FINE_LOCATION,Manifest.permission.ACCESS_COARSE_LOCATION);
        contacts=new Button(this);contacts.setText("Import host contacts into guest");
        contacts.setOnClickListener(v->{
            if(!HostBroker.permitted(this,Manifest.permission.READ_CONTACTS)) {
                pendingContacts=true;requestPermissions(new String[]{Manifest.permission.READ_CONTACTS},1);
            }
            else if(runtime!=null)runtime.importHostContacts();
        });layout.addView(contacts);
        Button inspect=new Button(this);inspect.setText("Check host bridges");inspect.setOnClickListener(v->refreshHost());layout.addView(inspect);
        network=new Button(this);network.setText("Test guest network (example.com)");
        network.setOnClickListener(v->{if(runtime!=null)runtime.checkHostBridges();});layout.addView(network);
        devices=new Button(this);devices.setText("Test camera, microphone and data bridges");
        devices.setOnClickListener(v->{if(runtime!=null)runtime.checkHostDevices();});layout.addView(devices);
        dialer=new Button(this);dialer.setText("Open host dialer from guest");
        dialer.setOnClickListener(v->{if(runtime!=null)runtime.openGuestDialer();});layout.addView(dialer);
        summary=new TextView(this);summary.setTextIsSelectable(true);layout.addView(summary);
        setContentView(scroll);refreshHost();
        onGuestChanged();
    }
    private void grant(LinearLayout layout,String name,String... permissions) {
        Button button=new Button(this);button.setText("Allow "+name);
        button.setOnClickListener(v->requestPermissions(permissions,1));layout.addView(button);
    }
    private void refreshHost() {
        if(reading || summary==null)return;reading=true;
        new Thread(()->{
            String value;
            try {value=HostBroker.info(this).toString(2);}catch(Exception error){value="Host query unavailable: "+error.getClass().getSimpleName();}
            final String text=value;runOnUiThread(()->{reading=false;if(!isDestroyed())summary.setText(text);});
        },"PocketHostStatus").start();
    }
    @Override public void onRequestPermissionsResult(int code,String[] permissions,int[] grants) {
        super.onRequestPermissionsResult(code,permissions,grants);refreshHost();
        if(!HostBroker.permitted(this,Manifest.permission.READ_CONTACTS))pendingContacts=false;
        onGuestChanged();
    }
    @Override public void onGuestChanged() {
        super.onGuestChanged();
        boolean active=runtime!=null && runtime.directBackend() && runtime.booted() && !runtime.stopping();
        if(contacts!=null)contacts.setEnabled(active);
        if(network!=null)network.setEnabled(active);
        if(devices!=null)devices.setEnabled(active);
        if(dialer!=null)dialer.setEnabled(active);
        if(active && pendingContacts && HostBroker.permitted(this,Manifest.permission.READ_CONTACTS)) {
            pendingContacts=false;runtime.importHostContacts();
        }
        if(runtime!=null && !runtime.directBackend())status.setText("Host bridges require the Android 9 profile. Select it in Manager after stopping Android 7.");
    }
}
