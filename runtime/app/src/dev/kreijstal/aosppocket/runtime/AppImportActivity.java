// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.app.Activity;
import android.content.*;
import android.content.pm.*;
import android.graphics.*;
import android.graphics.drawable.*;
import android.net.Uri;
import android.os.Bundle;
import android.text.*;
import android.text.format.Formatter;
import android.view.*;
import android.widget.*;
import java.io.*;
import java.util.*;
import java.util.concurrent.*;
import dev.kreijstal.aosppocket.bridge.AndroidVersions;

/** Host app/APK selection. Only the caller submits the confirmed package to the guest. */
public final class AppImportActivity extends Activity {
    private static final int BLUE = 0xff3478f6, INK = 0xff202735, MUTED = 0xff738094;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final List<Entry> apps = new ArrayList<>(), visible = new ArrayList<>();
    private GridView grid;
    private EditText search;
    private LinearLayout files, preview;
    private TextView message, previewName, previewDetails;
    private ImageView previewIcon;
    private Button appsTab, filesTab, install;
    private BaseAdapter adapter;
    private Entry selectedApp, fileEntry;
    private Uri fileUri;
    private boolean fileMode;
    private String restorePackage;
    private int guestSdk;

    private static final class Entry {
        final String name, packageName, version;
        final Bitmap icon;
        final long size;
        final int splits, minSdk;
        Entry(String name, String packageName, String version, Bitmap icon, long size, int splits, int minSdk) {
            this.name=name; this.packageName=packageName; this.version=version;
            this.icon=icon; this.size=size; this.splits=splits; this.minSdk=minSdk;
        }
    }

    @Override public void onCreate(Bundle state) {
        setTheme(android.R.style.Theme_Material_Light_NoActionBar);
        super.onCreate(state);
        guestSdk=getIntent().getIntExtra("guest_sdk",28);
        getWindow().setStatusBarColor(Color.WHITE);
        getWindow().setNavigationBarColor(Color.WHITE);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR |
                View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR);
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        LinearLayout page = new LinearLayout(this); page.setOrientation(LinearLayout.VERTICAL);
        page.setBackgroundColor(Color.WHITE); page.setPadding(dp(16),dp(8),dp(16),dp(12));
        LinearLayout header = new LinearLayout(this); header.setGravity(Gravity.CENTER_VERTICAL);
        Button back = new Button(this); back.setText("‹"); back.setTextSize(28);
        back.setContentDescription("Back"); back.setOnClickListener(v -> finish());
        header.addView(back,new LinearLayout.LayoutParams(dp(48),dp(48)));
        TextView title = text("Install apps",22,INK); title.setTypeface(null,Typeface.BOLD);
        header.addView(title); page.addView(header);
        LinearLayout tabs = new LinearLayout(this);
        appsTab = tab("Device apps",tabs, v -> mode(false));
        filesTab = tab("APK file",tabs, v -> mode(true)); page.addView(tabs);
        search = new EditText(this); search.setSingleLine(true); search.setTextSize(14);
        search.setHint("Search apps"); search.setPadding(dp(16),0,dp(16),0);
        search.setBackground(shape(0xfff3f5f8,0,12));
        LinearLayout.LayoutParams searchParams = new LinearLayout.LayoutParams(-1,dp(46));
        searchParams.setMargins(0,dp(12),0,dp(8)); page.addView(search,searchParams);
        search.addTextChangedListener(new TextWatcher() {
            public void beforeTextChanged(CharSequence s,int start,int count,int after) { }
            public void onTextChanged(CharSequence s,int start,int before,int count) { filter(); }
            public void afterTextChanged(Editable s) { }
        });
        FrameLayout content = new FrameLayout(this);
        page.addView(content,new LinearLayout.LayoutParams(-1,0,1));
        grid = new GridView(this); grid.setNumColumns(4); grid.setStretchMode(GridView.STRETCH_COLUMN_WIDTH);
        grid.setVerticalSpacing(dp(12)); grid.setHorizontalSpacing(dp(4));
        grid.setPadding(0,dp(8),0,dp(8)); grid.setClipToPadding(false);
        grid.setSelector(new android.graphics.drawable.ColorDrawable(Color.TRANSPARENT));
        adapter = new BaseAdapter() {
            public int getCount() { return visible.size(); }
            public Entry getItem(int position) { return visible.get(position); }
            public long getItemId(int position) { return position; }
            public View getView(int position,View old,ViewGroup parent) {
                Entry app = getItem(position);
                LinearLayout cell = new LinearLayout(AppImportActivity.this);
                cell.setOrientation(LinearLayout.VERTICAL); cell.setGravity(Gravity.TOP|Gravity.CENTER_HORIZONTAL);
                cell.setPadding(dp(2),dp(4),dp(2),dp(4));
                FrameLayout picture = new FrameLayout(AppImportActivity.this);
                ImageView icon = new ImageView(AppImportActivity.this); icon.setImageBitmap(app.icon);
                FrameLayout.LayoutParams iconParams = new FrameLayout.LayoutParams(dp(52),dp(52),Gravity.CENTER);
                picture.addView(icon,iconParams);
                TextView check = text(app==selectedApp ? "✓" : "",12,Color.WHITE);
                check.setGravity(Gravity.CENTER);
                check.setBackground(shape(app==selectedApp ? BLUE : Color.WHITE, app==selectedApp ? BLUE : 0xffd4dae3,20));
                picture.addView(check,new FrameLayout.LayoutParams(dp(20),dp(20),Gravity.TOP|Gravity.END));
                cell.addView(picture,new LinearLayout.LayoutParams(dp(68),dp(58)));
                TextView label = text(app.name,12,INK); label.setGravity(Gravity.TOP|Gravity.CENTER_HORIZONTAL);
                label.setMaxLines(2); label.setEllipsize(TextUtils.TruncateAt.END);
                LinearLayout.LayoutParams labelParams = new LinearLayout.LayoutParams(-1,dp(38));
                labelParams.topMargin=dp(6); cell.addView(label,labelParams);
                cell.setContentDescription(app.name+", "+app.packageName+(app==selectedApp ? ", selected" : ""));
                return cell;
            }
        };
        grid.setAdapter(adapter);
        grid.setOnItemClickListener((parent,view,position,id) -> {
            Entry item=visible.get(position); selectedApp=selectedApp==item ? null : item;
            adapter.notifyDataSetChanged(); updatePreview();
        });
        content.addView(grid,new FrameLayout.LayoutParams(-1,-1));
        message = text("Loading device apps…",14,MUTED); message.setGravity(Gravity.CENTER);
        content.addView(message,new FrameLayout.LayoutParams(-1,-1));
        files = new LinearLayout(this); files.setOrientation(LinearLayout.VERTICAL);
        files.setGravity(Gravity.CENTER); files.setPadding(dp(16),dp(16),dp(16),dp(16));
        TextView fileHint = text("Choose an APK to preview its icon and app details.",15,MUTED);
        fileHint.setGravity(Gravity.CENTER); files.addView(fileHint);
        Button browse = new Button(this); browse.setText("Choose APK file…");
        browse.setOnClickListener(v -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
                .setType("application/vnd.android.package-archive").addCategory(Intent.CATEGORY_OPENABLE),1));
        files.addView(browse); content.addView(files,new FrameLayout.LayoutParams(-1,-1));
        preview = new LinearLayout(this); preview.setGravity(Gravity.CENTER_VERTICAL);
        preview.setPadding(dp(12),dp(12),dp(12),dp(12)); preview.setBackground(shape(0xfff3f5f8,0,12));
        previewIcon=new ImageView(this); preview.addView(previewIcon,new LinearLayout.LayoutParams(dp(48),dp(48)));
        LinearLayout details=new LinearLayout(this); details.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams detailsParams=new LinearLayout.LayoutParams(0,-2,1); detailsParams.leftMargin=dp(12);
        preview.addView(details,detailsParams);
        previewName=text("",16,INK); previewName.setTypeface(null,Typeface.BOLD);
        previewName.setMaxLines(1); previewName.setEllipsize(TextUtils.TruncateAt.END); details.addView(previewName);
        previewDetails=text("",12,MUTED); details.addView(previewDetails);
        page.addView(preview,new LinearLayout.LayoutParams(-1,-2));
        install=new Button(this); install.setAllCaps(false); install.setTextSize(16);
        install.setTextColor(Color.WHITE); install.setOnClickListener(v -> confirm());
        LinearLayout.LayoutParams installParams=new LinearLayout.LayoutParams(-1,dp(48)); installParams.topMargin=dp(12);
        page.addView(install,installParams); setContentView(page);
        if(state!=null) {
            restorePackage=state.getString("package");
            String uri=state.getString("file_uri"); if(uri!=null) readApk(Uri.parse(uri));
            search.setText(state.getString("query",""));
        }
        mode(state!=null && state.getBoolean("file_mode"));
        worker.execute(() -> {
            try {
                PackageManager pm=getPackageManager(); List<Entry> loaded=new ArrayList<>();
                for(PackageInfo info:pm.getInstalledPackages(0)) {
                    if(Thread.currentThread().isInterrupted()) return;
                    ApplicationInfo app=info.applicationInfo;
                    if(app==null || info.packageName.equals(getPackageName())) continue;
                    if((app.flags&ApplicationInfo.FLAG_SYSTEM)!=0 && pm.getLaunchIntentForPackage(info.packageName)==null) continue;
                    try { loaded.add(entry(info)); } catch(RuntimeException disappeared) { /* Uninstalled during listing. */ }
                }
                Collections.sort(loaded,(a,b) -> a.name.compareToIgnoreCase(b.name));
                runOnUiThread(() -> {
                    if(isDestroyed() || isFinishing()) return;
                    apps.addAll(loaded);
                    for(Entry app:apps) if(app.packageName.equals(restorePackage)) selectedApp=app;
                    filter(); updatePreview();
                });
            } catch(RuntimeException failure) {
                runOnUiThread(() -> { if(!isDestroyed()) message.setText("Cannot list apps: "+failure.getMessage()); });
            }
        });
    }

    private Entry entry(PackageInfo info) {
        ApplicationInfo app=info.applicationInfo; PackageManager pm=getPackageManager();
        long size=new File(app.sourceDir).length();
        int splits=app.splitSourceDirs==null ? 0 : app.splitSourceDirs.length;
        if(app.splitSourceDirs!=null) for(String path:app.splitSourceDirs) size+=new File(path).length();
        Drawable drawable;
        try { drawable=app.loadIcon(pm); } catch(RuntimeException missing) { drawable=pm.getDefaultActivityIcon(); }
        Bitmap icon=Bitmap.createBitmap(dp(56),dp(56),Bitmap.Config.ARGB_8888);
        drawable.setBounds(0,0,icon.getWidth(),icon.getHeight()); drawable.draw(new Canvas(icon));
        return new Entry(app.loadLabel(pm).toString(),info.packageName,info.versionName,icon,size,splits,app.minSdkVersion);
    }
    private void filter() {
        if(adapter==null) return;
        String query=search.getText().toString().trim().toLowerCase(Locale.ROOT);
        visible.clear();
        for(Entry app:apps) if(app.name.toLowerCase(Locale.ROOT).contains(query) || app.packageName.toLowerCase(Locale.ROOT).contains(query)) visible.add(app);
        adapter.notifyDataSetChanged(); message.setText(visible.isEmpty() ? "No matching apps" : "");
        message.setVisibility(!fileMode && visible.isEmpty() ? View.VISIBLE : View.GONE);
    }
    private void mode(boolean file) {
        fileMode=file;
        appsTab.setTextColor(file ? MUTED : BLUE); filesTab.setTextColor(file ? BLUE : MUTED);
        appsTab.setBackground(shape(file ? Color.WHITE : 0xffedf3ff,0,10));
        filesTab.setBackground(shape(file ? 0xffedf3ff : Color.WHITE,0,10));
        grid.setVisibility(file ? View.GONE : View.VISIBLE); search.setVisibility(file ? View.GONE : View.VISIBLE);
        files.setVisibility(file ? View.VISIBLE : View.GONE);
        message.setVisibility(!file && visible.isEmpty() ? View.VISIBLE : View.GONE); updatePreview();
    }
    private void updatePreview() {
        Entry chosen=fileMode ? fileEntry : selectedApp;
        preview.setVisibility(chosen==null ? View.GONE : View.VISIBLE);
        boolean compatible=chosen!=null && chosen.minSdk<=guestSdk;
        install.setEnabled(compatible);
        install.setBackground(shape(compatible ? BLUE : 0xffb7c9eb,0,12));
        install.setText(chosen==null ? "Select an app to install" : compatible ? "Install in Android" : "Needs a newer Android version");
        if(chosen==null) return;
        previewIcon.setImageBitmap(chosen.icon); previewIcon.setContentDescription(chosen.name+" icon preview");
        previewName.setText(chosen.name);
        previewDetails.setText(chosen.packageName+"\n"+(chosen.version==null ? "" : chosen.version+" · ")+
                Formatter.formatShortFileSize(this,chosen.size)+(chosen.splits==0 ? "" : " · "+chosen.splits+" splits")+
                (compatible ? "" : "\n"+AndroidVersions.requirement(chosen.minSdk,guestSdk)));
        previewDetails.setTextColor(compatible ? MUTED : 0xffa12622);
    }
    @Override protected void onActivityResult(int request,int result,Intent data) {
        super.onActivityResult(request,result,data);
        if(request==1 && result==RESULT_OK && data!=null && data.getData()!=null) readApk(data.getData());
    }
    private void readApk(Uri uri) {
        fileUri=uri; fileEntry=null; updatePreview();
        TextView hint=(TextView)files.getChildAt(0); hint.setText("Reading APK preview…");
        worker.execute(() -> {
            File temp=null;
            try {
                temp=File.createTempFile("apk-preview-",".apk",getCacheDir());
                try(InputStream in=getContentResolver().openInputStream(uri); OutputStream out=new FileOutputStream(temp)) {
                    if(in==null) throw new IOException("Cannot open the selected file");
                    byte[] buffer=new byte[65536]; long size=0; int n;
                    while((n=in.read(buffer))!=-1) {
                        if(Thread.currentThread().isInterrupted()) return;
                        size+=n; if(size>2L*1024*1024*1024) throw new IOException("APK exceeds 2 GB");
                        out.write(buffer,0,n);
                    }
                }
                PackageInfo info=getPackageManager().getPackageArchiveInfo(temp.getAbsolutePath(),0);
                if(info==null || info.applicationInfo==null) throw new IOException("This file is not a readable APK");
                // Archive resource IDs resolve only when both paths point at the chosen APK.
                info.applicationInfo.sourceDir=temp.getAbsolutePath();
                info.applicationInfo.publicSourceDir=temp.getAbsolutePath();
                Entry parsed=entry(info);
                runOnUiThread(() -> {
                    if(isDestroyed() || !uri.equals(fileUri)) return;
                    fileEntry=parsed; hint.setText("Ready to install this APK."); updatePreview();
                });
            } catch(Exception failure) {
                runOnUiThread(() -> {
                    if(isDestroyed() || !uri.equals(fileUri)) return;
                    hint.setText("Cannot preview APK: "+failure.getMessage()); updatePreview();
                });
            } finally { if(temp!=null) temp.delete(); }
        });
    }
    private void confirm() {
        Entry chosen=fileMode ? fileEntry : selectedApp;
        if(chosen==null || chosen.minSdk>guestSdk)return;
        Intent result=new Intent();
        if(fileMode) {
            if(fileEntry==null || fileUri==null) return;
            result.setData(fileUri).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            result.setClipData(ClipData.newRawUri("APK",fileUri));
        } else {
            if(selectedApp==null) return;
            result.putExtra("host_package",selectedApp.packageName);
        }
        setResult(RESULT_OK,result); finish();
    }
    @Override protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        state.putBoolean("file_mode",fileMode); state.putString("query",search.getText().toString());
        if(selectedApp!=null) state.putString("package",selectedApp.packageName);
        if(fileUri!=null) state.putString("file_uri",fileUri.toString());
    }
    @Override protected void onDestroy() { worker.shutdownNow(); super.onDestroy(); }
    private int dp(int n) { return Math.round(n*getResources().getDisplayMetrics().density); }
    private TextView text(String value,int size,int color) {
        TextView view=new TextView(this); view.setText(value); view.setTextSize(size); view.setTextColor(color); return view;
    }
    private GradientDrawable shape(int color,int stroke,int radius) {
        GradientDrawable shape=new GradientDrawable(); shape.setColor(color); shape.setCornerRadius(dp(radius));
        if(stroke!=0) shape.setStroke(dp(1),stroke); return shape;
    }
    private Button tab(String title,LinearLayout parent,View.OnClickListener listener) {
        Button button=new Button(this); button.setText(title); button.setAllCaps(false); button.setOnClickListener(listener);
        parent.addView(button,new LinearLayout.LayoutParams(0,dp(44),1)); return button;
    }
}
