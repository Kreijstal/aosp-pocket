// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.app.*;
import android.content.*;
import android.net.Uri;
import android.os.*;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.WindowManager;
import java.io.*;
import java.util.concurrent.CopyOnWriteArraySet;
import dev.kreijstal.aosppocket.bridge.DisplayGeometry;

/** Owns one guest independently of the manager, full-screen or floating UI. */
public final class GuestService extends Service {
    interface Listener { void onGuestChanged(); }
    interface FrameListener {
        int displayPriority();
        void onDisplayOwnership(boolean owned);
        void onGuestFrame();
    }
    public final class LocalBinder extends Binder { GuestService get() { return GuestService.this; } }
    private static final String START = "dev.kreijstal.aosppocket.START";
    private static final String STOP = "dev.kreijstal.aosppocket.STOP";
    private static final String IMPORT = "dev.kreijstal.aosppocket.IMPORT";
    private static final String ADMIN = "dev.kreijstal.aosppocket.ADMIN";
    private static final String CHANNEL = "guest-runtime";
    private static final int NOTIFICATION = 1;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final CopyOnWriteArraySet<Listener> listeners = new CopyOnWriteArraySet<>();
    private final LocalBinder binder = new LocalBinder();
    private final GuestBinder receiver = new GuestBinder();
    private HandlerThread receiverThread;
    private volatile java.lang.Process guest;
    private boolean starting, stopping, initialized, foreground, booted, bundled, destroyed;
    private Thread imageWorker;
    private String message;
    int width, height, dpi;
    private int sdk, legacyWidth, legacyHeight, legacyDpi;
    private DisplayGeometry hostDisplay;
    private int hostRefreshHz = 60;
    private DirectSession direct;
    private AdminServer admin;
    private String adminError;
    private long adminGeneration;
    private final java.util.Set<FrameListener> visibleDisplays = new java.util.LinkedHashSet<>();
    private FrameListener displayOwner;
    boolean directBackend() { return sdk == 28; }
    boolean adminEnabled() { return admin != null; }
    String adminStatus() {
        return admin==null ? adminError==null?"Termux administration is off.":adminError
            : "Termux administration: 127.0.0.1:"+admin.port()+" · guest root console";
    }
    String adminToken() {
        if(admin==null)return null;
        return getSharedPreferences("guest-admin",MODE_PRIVATE).getString("key",null);
    }
    private byte[] adminKey(boolean rotate) {
        SharedPreferences prefs=getSharedPreferences("guest-admin",MODE_PRIVATE);
        String stored=prefs.getString("key","");byte[] key=new byte[32];
        if(rotate || !stored.matches("[0-9a-f]{64}")) {
            new java.security.SecureRandom().nextBytes(key);StringBuilder hex=new StringBuilder();
            for(byte value:key)hex.append(String.format(java.util.Locale.ROOT,"%02x",value&255));
            if(!prefs.edit().putString("key",hex.toString()).commit())throw new IllegalStateException("Cannot save admin key");
        } else for(int i=0;i<key.length;i++)key[i]=(byte)Integer.parseInt(stored.substring(i*2,i*2+2),16);
        return key;
    }
    void enableAdmin() {
        if(admin!=null)return;
        try{startForegroundService(new Intent(this,GuestService.class).setAction(ADMIN));}
        catch(RuntimeException error){adminError="Cannot enable Termux administration: "+error.getMessage();changed();}
    }
    void disableAdmin() {
        adminGeneration++;
        if(admin!=null){admin.close();admin=null;}adminError=null;changed();
        if(!running() && !starting && !preparing())releaseForeground();
    }
    void revokeAdmin() {
        adminGeneration++;
        boolean enabled=admin!=null;
        if(admin!=null){admin.close();admin=null;}
        try{adminKey(true);if(enabled)openAdmin();else changed();}
        catch(RuntimeException error){adminError="Cannot revoke admin key: "+error.getMessage();changed();}
        if(admin==null && !running() && !starting && !preparing())releaseForeground();
    }
    private <T>T adminMain(java.util.concurrent.Callable<T> work) throws Exception {
        java.util.concurrent.FutureTask<T> task=new java.util.concurrent.FutureTask<>(work);
        if(!main.post(task))throw new IOException("Guest service is stopping");
        try{return task.get(10,java.util.concurrent.TimeUnit.SECONDS);}
        catch(Exception error){task.cancel(false);throw error;}
    }
    private String adminState() throws org.json.JSONException {
        return new org.json.JSONObject().put("enabled",admin!=null).put("port",admin==null?0:admin.port())
            .put("sdk",sdk).put("running",running()).put("ready",booted).put("starting",starting)
            .put("stopping",stopping).put("status",message).put("shellUid",0).toString();
    }
    private void requireAdmin(long generation) throws IOException {
        if(destroyed || admin==null || generation!=adminGeneration)throw new IOException("Admin authorization was revoked");
    }
    private void openAdmin() {
        if(admin!=null)return;
        try {
            final long generation=++adminGeneration;
            admin=new AdminServer(AdminServer.DEFAULT_PORT,adminKey(false),new AdminServer.Backend(){
                @Override public String status() throws Exception{return adminMain(()->{requireAdmin(generation);return adminState();});}
                @Override public String control(boolean start) throws Exception {
                    return adminMain(()->{requireAdmin(generation);if(start)startGuest();else stopGuest();return adminState();});
                }
                @Override public java.lang.Process execute(String cwd,java.util.List<String> argv) throws Exception {
                    return adminMain(()->{
                        requireAdmin(generation);
                        // Owner recovery/file access must also work after a guest crash.
                        // Only service-dependent commands require a booted Android system.
                        if(!directBackend() || stopping ||
                            !new File(DirectImage.root(GuestService.this),"system/bin/sh").isFile())
                            throw new IOException("Prepare the Android 9 image before using its root console");
                        return DirectImage.admin(DirectImage.root(GuestService.this),cwd,argv);
                    });
                }
            });
            adminError=null;changed();
        } catch(IOException | RuntimeException error){adminError="Cannot open Termux administration: "+error.getMessage();changed();}
    }
    int selectedSdk() { return sdk; }
    boolean directAvailable() { return DirectImage.bundled(this); }
    android.graphics.Bitmap snapshot() { return direct==null ? null : direct.snapshot(); }
    void presentSurface(android.view.Surface value) {if(direct!=null)direct.surface(value);}
    void detachSurface(android.view.Surface value) {if(direct!=null)direct.detach(value);}
    void displayVisible(FrameListener id, boolean visible) {
        if (visible) visibleDisplays.add(id); else visibleDisplays.remove(id);
        FrameListener next = null;
        for (FrameListener candidate : visibleDisplays)
            if (next == null || candidate.displayPriority() >= next.displayPriority()) next = candidate;
        if (next != displayOwner) {
            FrameListener previous = displayOwner;
            displayOwner = next;
            // Revoke first: neither backend may present to two windows during a handoff.
            if (previous != null) previous.onDisplayOwnership(false);
            if (next != null) next.onDisplayOwnership(true);
            Log.i("PocketWindows", "Display owner=" + (next == null ? "none" :
                next.displayPriority() > 0 ? "Android window" : "Manager"));
        }
        if (direct != null) direct.visible(displayOwner != null);
    }
    void touch(android.view.MotionEvent event, float sx, float sy) { if (direct != null) direct.touch(event, sx, sy); }
    void key(int code) { if (direct != null) direct.key(code); }
    void installApps(java.util.Map<String,Uri> apks) { if (direct != null && booted) direct.install(apks); }
    void importHostContacts() { if (direct != null && booted) direct.importHostContacts(); }
    void checkHostBridges() { if (direct != null && booted) direct.checkHostBridges(); }
    void checkHostDevices() { if (direct != null && booted) direct.checkHostDevices(); }
    void openGuestDialer() { if (direct != null && booted) direct.openGuestDialer(); }
    boolean hasLastApp() { return direct != null && direct.hasLastApp(); }
    void openLastApp() { if (direct != null && booted) direct.openLastApp(); }
    void selectSdk(int value) {
        if (value == sdk || running() || starting || preparing() || stopping || value != 25 && value != 28) return;
        if (value == 28 && !directAvailable()) return;
        sdk = value;
        getSharedPreferences("guest-backend", MODE_PRIVATE).edit().putInt("sdk", sdk).apply();
        geometry();
        message("Android " + (sdk == 28 ? "9 · direct backend" : "7.1.1 · PRoot") + " selected. Tap Start.");
    }
    private void geometry() {
        width = sdk == 28 ? hostDisplay.width : legacyWidth;
        height = sdk == 28 ? hostDisplay.height : legacyHeight;
        dpi = sdk == 28 ? hostDisplay.densityDpi : legacyDpi;
    }
    private void bootDirect() {
        if (direct != null) return;
        receiver.reset();
        starting = true;
        direct = new DirectSession(this, new DisplayGeometry(width,height,dpi), hostRefreshHz, new DirectSession.Listener() {
            @Override public void status(String text) { if (!destroyed) message(text); }
            @Override public void ready() {
                starting = false; booted = true;
                message("Android 9 running · "+width+" × "+height);
            }
            @Override public void stopped(String text) {
                direct = null; starting = false; booted = false; stopping = false;
                if (!destroyed) { message(text); releaseForeground(); }
            }
        });
        direct.visible(displayOwner != null);
        direct.start();
        changed();
    }

    @Override public void onCreate() {
        super.onCreate();
        Native.configureHost();
        bundled = GuestImage.hasBundled(this);
        message = hasImage() ? "Android is ready. Tap Start." : bundled ?
            "Tap Start to set up the included Android 7.1.1. No download needed." : "Import a trusted guest ZIP, then Start.";
        DisplayMetrics metrics = new DisplayMetrics();
        android.view.Display display = ((WindowManager)getSystemService(WINDOW_SERVICE)).getDefaultDisplay();
        display.getRealMetrics(metrics);
        // Same default as VMOS: the guest runs at the host's refresh rate.
        float refresh = display.getRefreshRate();
        hostRefreshHz = refresh >= 1 && refresh <= 120 ? Math.round(refresh) : 60;
        hostDisplay=new DisplayGeometry(Math.min(metrics.widthPixels,metrics.heightPixels),
            Math.max(metrics.widthPixels,metrics.heightPixels),metrics.densityDpi);
        width = Math.min(metrics.widthPixels, metrics.heightPixels);
        // Reserve the full-screen viewer's 48dp control strip. Preview/PiP
        // surfaces keep this logical size and only scale its presentation.
        height = Math.max(metrics.widthPixels, metrics.heightPixels) - Math.round(48 * metrics.density);
        dpi = metrics.densityDpi;
        SharedPreferences geometry = getSharedPreferences("guest-display", MODE_PRIVATE);
        width = geometry.getInt("width", width);
        height = geometry.getInt("height", height);
        dpi = geometry.getInt("dpi", dpi);
        if (!geometry.contains("width")) geometry.edit().putInt("width", width)
            .putInt("height", height).putInt("dpi", dpi).apply();
        legacyWidth = width; legacyHeight = height; legacyDpi = dpi;
        sdk = getSharedPreferences("guest-backend", MODE_PRIVATE).getInt("sdk",
            new File(getFilesDir(), "rootfs/init").isFile() || !directAvailable() ? 25 : 28);
        geometry();
        message = "Android " + (sdk == 28 ? "9 · direct backend" : "7.1.1 · PRoot") + " ready. Tap Start.";
        receiverThread = new HandlerThread("GuestBinder");
        receiverThread.start();
        IntentFilter filter = new IntentFilter(GuestBinder.ACTION);
        Handler handler = new Handler(receiverThread.getLooper());
        if (Build.VERSION.SDK_INT >= 33)
            registerReceiver(receiver, filter, null, handler, Context.RECEIVER_NOT_EXPORTED);
        else registerReceiver(receiver, filter, null, handler);
        NotificationChannel channel = new NotificationChannel(CHANNEL, "Running Android guest", NotificationManager.IMPORTANCE_LOW);
        getSystemService(NotificationManager.class).createNotificationChannel(channel);
    }
    @Override public IBinder onBind(Intent intent) { return binder; }
    @Override public int onStartCommand(Intent intent, int flags, int id) {
        if (intent != null && ADMIN.equals(intent.getAction())) {
            promote();openAdmin();if(admin==null && !running() && !starting)releaseForeground();
        } else if (intent != null && STOP.equals(intent.getAction())) stopGuest();
        else if (intent != null && START.equals(intent.getAction())) {
            promote();
            if (directBackend() || hasImage()) boot();
            else if (imageWorker == null) prepareImage(null);
        } else if (intent != null && IMPORT.equals(intent.getAction()) && imageWorker == null) {
            promote();
            prepareImage(intent.getData());
        }
        return START_NOT_STICKY;
    }
    boolean hasImage() { if (directBackend()) return new File(getFilesDir(), "api28-rootfs/system/bin/pocket-init-api28").isFile(); return new File(getFilesDir(), "rootfs/init").isFile(); }
    boolean running() { return directBackend() ? direct != null && direct.running() : guest != null; }
    boolean starting() { return starting; }
    boolean preparing() { return imageWorker != null || direct != null && !booted; }
    boolean canStart() { return directBackend() ? directAvailable() : hasImage() || bundled; }
    boolean stopping() { return stopping; }
    boolean initialized() { return initialized; }
    boolean booted() { return booted; }
    String status() { return message; }
    void addListener(Listener listener) { listeners.add(listener); listener.onGuestChanged(); }
    void removeListener(Listener listener) { listeners.remove(listener); }
    void refresh() { changed(); }
    private void changed() { for (Listener listener : listeners) listener.onGuestChanged(); }
    private void message(String value) {
        message = value;
        Log.i("PocketRuntime", value);
        changed();
        if (foreground) getSystemService(NotificationManager.class).notify(NOTIFICATION, notification());
    }
    void startGuest() {
        if (running() || starting || stopping || preparing()) return;
        if (!canStart()) { message("Import a trusted guest ZIP in Pocket Manager first."); return; }
        starting = true;
        message("Starting Android…");
        try { startForegroundService(new Intent(this, GuestService.class).setAction(START)); }
        catch (RuntimeException error) { starting = false; message("Cannot start guest: " + error); }
    }
    void importGuest(Uri uri) {
        if (directBackend() || running() || starting || hasImage()) return;
        starting = true;
        message("Importing guest…");
        try { startForegroundService(new Intent(this, GuestService.class).setAction(IMPORT).setData(uri)); }
        catch (RuntimeException error) { starting = false; message("Cannot import guest: " + error); }
    }
    private void prepareImage(Uri uri) {
        final boolean defaultImage = uri == null;
        message(defaultImage ? "Setting up included Android…" : "Importing guest…");
        imageWorker = new Thread(() -> {
            File archive = defaultImage ? null : new File(getCacheDir(), "incoming-" + java.util.UUID.randomUUID() + ".zip");
            Exception failure = null;
            try {
                final int[] last = {-1};
                GuestImage.Progress progress = (completed, total) -> {
                    int percent = total == 0 ? 0 : (int)(completed * 100 / total);
                    if (percent == last[0]) return;
                    last[0] = percent;
                    main.post(() -> {
                        if (!destroyed && imageWorker != null && !stopping)
                            message((defaultImage ? "Setting up included Android: " : "Importing guest: ") + percent + "%");
                    });
                };
                if (defaultImage) GuestImage.installBundled(this, progress);
                else {
                    try (InputStream in = getContentResolver().openInputStream(uri);
                         OutputStream out = new FileOutputStream(archive)) {
                        if (in == null) throw new IOException("Cannot open selected image");
                        GuestImage.copy(in, out, 1024L * 1024 * 1024);
                    }
                    GuestImage.install(archive, getFilesDir(), progress);
                }
            } catch (Exception error) { failure = error; }
            finally { if (archive != null) archive.delete(); }
            final Exception result = failure;
            main.post(() -> {
                if (destroyed) return;
                imageWorker = null;
                boolean cancelled = stopping;
                stopping = false;
                if (cancelled || result != null) {
                    starting = false;
                    message(cancelled ? "Setup cancelled. Tap Start to retry." : "Setup failed: " + result.getMessage() + ". Tap Start to retry or import a guest.");
                    releaseForeground();
                } else if (defaultImage) {
                    message("Included Android installed. Starting…");
                    boot();
                } else {
                    starting = false;
                    message("Guest imported. Tap Start.");
                    releaseForeground();
                }
            });
        }, "GuestImageSetup");
        imageWorker.start();
        changed();
    }
    private Notification notification() {
        PendingIntent open = PendingIntent.getActivity(this, 0, GuestActivity.intent(this),
            PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        PendingIntent stop = PendingIntent.getService(this, 1,
            new Intent(this, GuestService.class).setAction(STOP),
            PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this, CHANNEL).setSmallIcon(android.R.drawable.ic_media_play)
            .setContentTitle("Pocket Android").setContentText(message).setContentIntent(open)
            .setOngoing(true).addAction(android.R.drawable.ic_media_pause, "Stop Android", stop).build();
    }
    private void promote() { startForeground(NOTIFICATION, notification()); foreground = true; }
    private void releaseForeground() {
        if(admin!=null){if(foreground)getSystemService(NotificationManager.class).notify(NOTIFICATION,notification());return;}
        stopForeground(true);foreground=false;stopSelf();
    }
    private void boot() {
        if (directBackend()) { bootDirect(); return; }
        if (guest != null) { starting = false; changed(); return; }
        try {
            File root = new File(getFilesDir(), "rootfs");
            new File(root, "dev/input").mkdirs();
            GuestImage.prepareRuntime(this, root);
            receiver.reset();
            GuestBinder.prepare(this, root);
            File script = new File(getFilesDir(), "guest.sh");
            try (InputStream in = getAssets().open("guest.sh"); OutputStream out = new FileOutputStream(script)) {
                GuestImage.copy(in, out, 65536);
            }
            Native.initialize(getFilesDir().getPath(), width, height, dpi);
            initialized = true;
            ProcessBuilder builder = new ProcessBuilder("/system/bin/sh", script.getPath(),
                getFilesDir().getPath(), getApplicationInfo().nativeLibraryDir + "/libproot.so");
            builder.environment().put("POCKET_PARENT_PID", Integer.toString(android.os.Process.myPid()));
            builder.redirectErrorStream(true);
            builder.redirectOutput(new File(getFilesDir(), "proot.log"));
            guest = builder.start();
            final java.lang.Process current = guest;
            starting = false;
            booted = false;
            message("Android starting · guest stays running when its windows close");
            new Thread(() -> {
                File marker = new File(root, "data/pocket-boot-complete");
                for (int attempt = 0; attempt < 300 && guest == current && current.isAlive(); attempt++) {
                    if (guestBootComplete()) {
                        main.post(() -> {
                            if (guest == current && current.isAlive()) {
                                try (OutputStream out = new FileOutputStream(marker)) { out.write('1'); }
                                catch (IOException error) { Log.e("PocketRuntime", "Cannot save guest status", error); }
                                booted = true;
                                message("Android running · " + width + " × " + height);
                            }
                        });
                        return;
                    }
                    try { Thread.sleep(2000); }
                    catch (InterruptedException error) { Thread.currentThread().interrupt(); return; }
                }
            }, "GuestBootStatus").start();
            new Thread(() -> {
                try {
                    int code = current.waitFor();
                    main.post(() -> {
                        if (guest == current) {
                            guest = null;
                            booted = false;
                            boolean requested = stopping;
                            stopping = false;
                            message(requested ? "Android stopped. Tap Start to boot again." :
                                "Android exited: " + code + (code == 137 ? ". Check the native-process cap." : ". See proot.log."));
                            releaseForeground();
                        }
                    });
                } catch (InterruptedException error) { Thread.currentThread().interrupt(); }
            }, "GuestWait").start();
        } catch (Exception error) {
            starting = false;
            message("Start failed: " + error);
            releaseForeground();
        }
    }
    private boolean guestBootComplete() {
        java.lang.Process query = null;
        try {
            File root = new File(getFilesDir(), "rootfs");
            ProcessBuilder builder = new ProcessBuilder(getApplicationInfo().nativeLibraryDir + "/libproot.so",
                "--kill-on-exit", "-r", root.getPath(), "-0", "-w", "/", "-b", "/dev", "-b", "/proc",
                "-b", new File(root, "dev/__properties__").getPath() + ":/dev/__properties__",
                "/system/bin/getprop", "sys.boot_completed");
            builder.environment().put("PROOT_NO_SECCOMP", "1");
            builder.environment().put("POCKET_PARENT_PID", Integer.toString(android.os.Process.myPid()));
            builder.environment().put("PROOT_TMP_DIR", new File(getFilesDir(), "tmp").getPath());
            builder.environment().remove("LD_PRELOAD");
            builder.environment().remove("LD_LIBRARY_PATH");
            builder.redirectError(new File(getFilesDir(), "boot-query.log"));
            query = builder.start();
            if (!query.waitFor(8, java.util.concurrent.TimeUnit.SECONDS)) return false;
            ByteArrayOutputStream value = new ByteArrayOutputStream();
            GuestImage.copy(query.getInputStream(), value, 128);
            return query.exitValue() == 0 && "1".equals(value.toString("UTF-8").trim());
        } catch (Exception error) { Log.w("PocketRuntime", "Guest property query failed", error); return false; }
        finally { if (query != null && query.isAlive()) query.destroyForcibly(); }
    }
    void stopGuest() {
        if (direct != null) { stopping = true; message("Stopping Android 9…"); direct.close(); return; }
        if (imageWorker != null && !stopping) {
            stopping = true;
            message("Cancelling image setup…");
            imageWorker.interrupt();
        } else if (guest != null && !stopping) {
            stopping = true;
            message("Stopping Android…");
            guest.destroyForcibly(); // Patched PRoot honors Android's SIGTERM.
        } else if (guest == null) { starting = false; releaseForeground(); }
    }
    @Override public void onDestroy() {
        destroyed = true;
        adminGeneration++;
        if(admin!=null){admin.close();admin=null;}
        visibleDisplays.clear();
        if (displayOwner != null) displayOwner.onDisplayOwnership(false);
        displayOwner = null;
        if (direct != null) direct.close();
        if (imageWorker != null) imageWorker.interrupt();
        if (guest != null) guest.destroyForcibly();
        unregisterReceiver(receiver);
        receiverThread.quitSafely();
        listeners.clear();
        super.onDestroy();
    }
}
