// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.app.ActivityManager;
import android.content.*;
import android.content.pm.ApplicationInfo;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.widget.*;
import java.io.File;
import java.util.*;
import dev.kreijstal.aosppocket.bridge.AndroidVersions;

/** Management lives on its own page, away from the guest display. */
public final class SettingsActivity extends RuntimeActivity {
    private Button install, start, stop, apk, lastApp;
    private Spinner version, attestation;
    private final List<Integer> versionSdks=new ArrayList<>();
    private TextView performance;
    private TextView adminStatus;
    private Button adminToggle,adminCopy,adminRevoke;
    private String performanceReport;
    private boolean updating;
    private android.net.Uri pendingImport, pendingApk;
    private String pendingHostPackage;
    @Override public void onCreate(Bundle state) {
        setTheme(android.R.style.Theme_Material_Light_NoActionBar);
        super.onCreate(state);
        autoStart = getIntent().getBooleanExtra("start_guest", false);
        setTaskDescription(new ActivityManager.TaskDescription("Pocket Manager", null, 0xff24465e));
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(dp(12), dp(8), dp(12), dp(8));
        Button androidPage=new Button(this); androidPage.setText("Back to Android");
        androidPage.setOnClickListener(v->androidPage()); layout.addView(androidPage);
        TextView heading=new TextView(this); heading.setText("Settings"); heading.setTextSize(24); layout.addView(heading);
        TextView attestationHeading=new TextView(this);
        attestationHeading.setText("Attestation backend");
        attestationHeading.setTextSize(18);
        layout.addView(attestationHeading);
        attestation = new Spinner(this);
        attestation.setAdapter(new ArrayAdapter<String>(this, android.R.layout.simple_spinner_dropdown_item,
            new ArrayList<>(AttestationBackend.LABELS)));
        attestation.setSelection(Math.max(0,AttestationBackend.VALUES.indexOf(AttestationBackend.get(this))), false);
        attestation.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> parent, android.view.View view, int position, long id) {
                if (updating || runtime == null) return;
                String value = AttestationBackend.VALUES.get(position);
                if (value.equals(runtime.attestationBackend())) return;
                runtime.setAttestationBackend(value);
                // Keys are bound to the identity the guest booted with, so a
                // running guest would otherwise keep attesting the old way.
                if (runtime.running()) runtime.stopGuest();
                Toast.makeText(SettingsActivity.this,
                    AttestationBackend.label(value)+" applies the next time Android starts.",Toast.LENGTH_LONG).show();
            }
            @Override public void onNothingSelected(AdapterView<?> parent) { }
        });
        layout.addView(attestation);
        Button resetKeystore=new Button(this);
        resetKeystore.setText("Reset guest keystore");
        resetKeystore.setOnClickListener(v->{
            if (runtime == null) return;
            if (runtime.running()) {
                Toast.makeText(this,"Stop Android before resetting the guest keystore.",Toast.LENGTH_LONG).show();
                return;
            }
            new android.app.AlertDialog.Builder(this)
                .setTitle("Reset guest keystore?")
                .setMessage("Every app inside Android loses the keys it stored in the Android Keystore.\n\n"
                    +"Apps that encrypt data with those keys can lose that data or sign you out: "
                    +"saved logins, tickets, and message history may become unreadable.\n\n"
                    +"Only do this after changing the attestation backend. "
                    +"The previous keystore is kept so the reset can be undone while Android is stopped.")
                .setNegativeButton("Cancel",null)
                .setPositiveButton("Reset keystore",(dialog,which)->{
                    try {
                        File backup=runtime.resetGuestKeystore();
                        Toast.makeText(this,backup==null?"The guest keystore was already empty."
                            :"Guest keystore reset. Previous keys saved in /data/misc/"+backup.getName()+".",
                            Toast.LENGTH_LONG).show();
                    } catch(Exception error){Toast.makeText(this,"Cannot reset guest keystore: "+error.getMessage(),Toast.LENGTH_LONG).show();}
                })
                .show();
        });
        layout.addView(resetKeystore);
        status = new TextView(this);
        status.setLines(2);
        layout.addView(status);
        version = new Spinner(this);
        List<String> names=new ArrayList<>();
        versionSdks.add(25);names.add("Android 7.1.1 · PRoot");
        versionSdks.add(28);names.add("Android 9 · direct backend");
        if(DirectImage.bundled(this,29)){versionSdks.add(29);names.add("Android 10");}
        if(DirectImage.bundled(this,32)){versionSdks.add(32);names.add("Android 12L · experimental");}
        version.setAdapter(new ArrayAdapter<String>(this, android.R.layout.simple_spinner_dropdown_item,names));
        version.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> parent, android.view.View view, int position, long id) {
                if (!updating && runtime != null) runtime.selectSdk(versionSdks.get(position));
            }
            @Override public void onNothingSelected(AdapterView<?> parent) { }
        });
        layout.addView(version);
        LinearLayout controls = new LinearLayout(this);
        install = button("Import guest", controls, v -> importGuest());
        start = button("Start", controls, v -> requestStart());
        stop = button("Stop", controls, v -> { if (runtime != null) runtime.stopGuest(); });
        lastApp = button("Open last app", controls, v -> { if (runtime != null) { runtime.openLastApp(); androidPage(); } });
        layout.addView(controls, new LinearLayout.LayoutParams(-1, dp(48)));
        Button peripherals = new Button(this);
        peripherals.setText("Host peripherals");
        peripherals.setOnClickListener(v -> startActivity(new Intent(this, PeripheralsActivity.class)));
        layout.addView(peripherals, new LinearLayout.LayoutParams(-1, dp(44)));
        LinearLayout open = new LinearLayout(this);
        apk = button("Install APK", open, v -> chooseApk());
        button("Android page", open, v -> androidPage());
        button("Close manager", open, v -> finishAndRemoveTask());
        layout.addView(open, new LinearLayout.LayoutParams(-1, dp(52)));
        TextView note = new TextView(this);
        note.setText("Android has its own launcher icon and Recents window. Closing this manager keeps it running. Shared host UID/kernel. The attestation backend is fixed when Android starts; changing it stops the guest, and existing keys keep the identity they were created with until the guest keystore is reset.");
        note.setTextColor(Color.DKGRAY);
        layout.addView(note);
        adminStatus=new TextView(this);layout.addView(adminStatus);
        adminToggle=new Button(this);adminToggle.setText("Enable Termux administration");
        adminToggle.setOnClickListener(v->{if(runtime!=null){if(runtime.adminEnabled())runtime.disableAdmin();else runtime.enableAdmin();}});
        layout.addView(adminToggle);
        adminCopy=new Button(this);adminCopy.setText("Copy connection token");
        adminCopy.setOnClickListener(v->{
            String token=runtime==null?null:runtime.adminToken();
            if(token!=null){
                ClipData clip=ClipData.newPlainText("Pocket admin connection token",token);
                android.os.PersistableBundle extras=new android.os.PersistableBundle();
                extras.putBoolean("android.content.extra.IS_SENSITIVE",true);clip.getDescription().setExtras(extras);
                ((android.content.ClipboardManager)getSystemService(CLIPBOARD_SERVICE)).setPrimaryClip(clip);
                Toast.makeText(this,"Paste this token into pocketctl pair in Termux",Toast.LENGTH_LONG).show();
            }
        });layout.addView(adminCopy);
        Button saveClient=new Button(this);saveClient.setText("Save Termux client");
        saveClient.setOnClickListener(v->startActivityForResult(new Intent(Intent.ACTION_CREATE_DOCUMENT)
            .setType("text/x-python").addCategory(Intent.CATEGORY_OPENABLE).putExtra(Intent.EXTRA_TITLE,"pocketctl.py"),3));
        layout.addView(saveClient);
        adminRevoke=new Button(this);adminRevoke.setText("Revoke connection token");
        adminRevoke.setOnClickListener(v->{if(runtime!=null)runtime.revokeAdmin();});layout.addView(adminRevoke);
        TextView adminHelp=new TextView(this);
        adminHelp.setText("Enable only when you want Termux to administer this guest. In Termux: python pocketctl.py pair, then python pocketctl.py shell. No Samsung root required. Disabling closes admin sessions; revoking requires pairing again. Administration is off after Pocket restarts.");
        layout.addView(adminHelp);
        performance=new TextView(this);performance.setTextColor(Color.DKGRAY);layout.addView(performance);
        Button copyPerformance=new Button(this);copyPerformance.setText("Copy display report");
        copyPerformance.setOnClickListener(v->{
            readPerformance();
            if(performanceReport!=null) {
                android.content.ClipboardManager clipboard=(android.content.ClipboardManager)getSystemService(CLIPBOARD_SERVICE);
                clipboard.setPrimaryClip(ClipData.newPlainText("Pocket display timing",performanceReport));
                Toast.makeText(this,"Display report copied",Toast.LENGTH_SHORT).show();
            }
        });layout.addView(copyPerformance);
        ScrollView scroll=new ScrollView(this); scroll.addView(layout); setContentView(scroll);
        onGuestChanged();
    }
    @Override protected void onResume(){super.onResume();readPerformance();}
    private void readPerformance() {
        if(performance==null)return;
        try {
            performanceReport=new String(java.nio.file.Files.readAllBytes(new java.io.File(DirectImage.root(this,runtime!=null && runtime.directBackend()?runtime.selectedSdk():28),"logs/display-performance.json").toPath()),"UTF-8");
            org.json.JSONObject report=new org.json.JSONObject(performanceReport);
            performance.setText(String.format(java.util.Locale.ROOT,
                "Last visible display sample: %.1f FPS · %d × %d\nCompositor copy: %.1f ms · Present: %.1f ms\nPresenter: %s\nHost renderer: %s\nGuest rendering: SwiftShader",
                report.getDouble("interval_fps"),report.getInt("width"),report.getInt("height"),
                report.getDouble("mean_capture_ms"),report.getDouble("mean_present_ms"),
                report.optString("presenter","unknown"),report.optString("host_gl_renderer","unknown")));
        } catch(Exception unavailable) {performanceReport=null;performance.setText("Display timing appears after Android 9 has been visible for a few seconds.");}
    }
    @Override public void onGuestChanged() {
        super.onGuestChanged();
        if (start == null) return;
        if (runtime != null && pendingImport != null) {
            android.net.Uri uri = pendingImport;
            pendingImport = null;
            runtime.importGuest(uri);
        }
        if (runtime != null && pendingHostPackage != null) {
            String name = pendingHostPackage; pendingHostPackage = null;
            try { installHostApp(getPackageManager().getApplicationInfo(name, 0)); }
            catch (android.content.pm.PackageManager.NameNotFoundException missing) {
                Toast.makeText(this, "This app is no longer installed on the device", Toast.LENGTH_LONG).show();
            }
        }
        if (runtime != null && pendingApk != null) {
            Uri uri = pendingApk; pendingApk = null;
            runtime.installApps(Collections.singletonMap("base.apk", uri));
            androidPage();
        }
        boolean ready = runtime != null;
        if(adminToggle!=null){
            boolean enabled=ready && runtime.adminEnabled();
            adminStatus.setText(ready?runtime.adminStatus():"Connecting…");
            adminToggle.setEnabled(ready);adminToggle.setText(enabled?"Disable Termux administration":"Enable Termux administration");
            adminCopy.setEnabled(enabled);adminRevoke.setEnabled(ready);
        }
        updating = true;
        if (ready) version.setSelection(Math.max(0,versionSdks.indexOf(runtime.selectedSdk())), false);
        version.setEnabled(ready && !runtime.running() && !runtime.starting() && !runtime.preparing() && !runtime.stopping());
        if (attestation != null) attestation.setSelection(
            Math.max(0,AttestationBackend.VALUES.indexOf(runtime == null ? AttestationBackend.get(this) : runtime.attestationBackend())), false);
        updating = false;
        apk.setEnabled(ready && runtime.directBackend() && runtime.booted() && !runtime.stopping());
        lastApp.setEnabled(ready && runtime.directBackend() && runtime.booted() && !runtime.stopping() && runtime.hasLastApp());
        install.setEnabled(ready && !runtime.directBackend() && !runtime.hasImage() && !runtime.starting() && !runtime.running());
        start.setEnabled(ready && runtime.canStart() && !runtime.running() && !runtime.starting() && !runtime.stopping());
        stop.setEnabled(ready && (runtime.running() || runtime.preparing()) && !runtime.stopping());
    }
    private void androidPage() {
        startActivity(new Intent(this,MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP|Intent.FLAG_ACTIVITY_SINGLE_TOP|Intent.FLAG_ACTIVITY_NO_USER_ACTION));
        finish();
    }
    /** Icon-grid picker and an APK preview before submitting anything to the guest. */
    private void chooseApk() {
        startActivityForResult(new Intent(this, AppImportActivity.class)
            .putExtra("guest_sdk",runtime.selectedSdk()), 2);
    }
    private void installHostApp(ApplicationInfo app) {
        if(app.minSdkVersion>runtime.selectedSdk()) {
            new android.app.AlertDialog.Builder(this).setTitle("App needs a newer Android version")
                .setMessage(AndroidVersions.requirement(app.minSdkVersion,runtime.selectedSdk()))
                .setPositiveButton(android.R.string.ok,null).show();
            return;
        }
        Map<String, Uri> apks = new LinkedHashMap<>();
        apks.put("base.apk", Uri.fromFile(new File(app.sourceDir)));
        if (app.splitSourceDirs != null)
            for (String split : app.splitSourceDirs) apks.put(new File(split).getName(), Uri.fromFile(new File(split)));
        runtime.installApps(apks);
        androidPage();
    }
    private void importGuest() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("application/zip");
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        startActivityForResult(intent, 1);
    }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null) return;
        if(request==3 && data.getData()!=null){
            try(java.io.InputStream in=getAssets().open("pocketctl.py");
                java.io.OutputStream out=getContentResolver().openOutputStream(data.getData())){
                if(out==null)throw new java.io.IOException("Cannot open client destination");
                byte[] bytes=new byte[65536];int n;while((n=in.read(bytes))!=-1)out.write(bytes,0,n);
                Toast.makeText(this,"Termux client saved",Toast.LENGTH_SHORT).show();
            }catch(java.io.IOException error){Toast.makeText(this,"Cannot save client: "+error.getMessage(),Toast.LENGTH_LONG).show();}
            return;
        }
        if (request == 2) {
            String hostPackage = data.getStringExtra("host_package");
            if (hostPackage != null) {
                pendingHostPackage = hostPackage;
                onGuestChanged();
                return;
            }
            if (data.getData() == null) return;
            pendingApk = data.getData();
            onGuestChanged();
            return;
        }
        if (request != 1 || data.getData() == null) return;
        pendingImport = data.getData();
        onGuestChanged();
    }
}
