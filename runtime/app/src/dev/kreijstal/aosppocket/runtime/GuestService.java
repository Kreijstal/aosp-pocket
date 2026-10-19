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

/** Owns one guest independently of the manager, full-screen or floating UI. */
public final class GuestService extends Service {
    interface Listener { void onGuestChanged(); }
    public final class LocalBinder extends Binder { GuestService get() { return GuestService.this; } }
    private static final String START = "dev.kreijstal.aosppocket.START";
    private static final String STOP = "dev.kreijstal.aosppocket.STOP";
    private static final String IMPORT = "dev.kreijstal.aosppocket.IMPORT";
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

    @Override public void onCreate() {
        super.onCreate();
        Native.configureHost();
        bundled = GuestImage.hasBundled(this);
        message = hasImage() ? "Android is ready. Tap Start." : bundled ?
            "Tap Start to set up the included Android 7.1.1. No download needed." : "Import a trusted guest ZIP, then Start.";
        DisplayMetrics metrics = new DisplayMetrics();
        ((WindowManager)getSystemService(WINDOW_SERVICE)).getDefaultDisplay().getRealMetrics(metrics);
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
        if (intent != null && STOP.equals(intent.getAction())) stopGuest();
        else if (intent != null && START.equals(intent.getAction())) {
            promote();
            if (hasImage()) boot();
            else if (imageWorker == null) prepareImage(null);
        } else if (intent != null && IMPORT.equals(intent.getAction()) && imageWorker == null) {
            promote();
            prepareImage(intent.getData());
        }
        return START_NOT_STICKY;
    }
    boolean hasImage() { return new File(getFilesDir(), "rootfs/init").isFile(); }
    boolean running() { return guest != null; }
    boolean starting() { return starting; }
    boolean preparing() { return imageWorker != null; }
    boolean canStart() { return hasImage() || bundled; }
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
        if (running() || starting) return;
        if (!canStart()) { message("Import a trusted guest ZIP in Pocket Manager first."); return; }
        starting = true;
        message("Starting Android…");
        try { startForegroundService(new Intent(this, GuestService.class).setAction(START)); }
        catch (RuntimeException error) { starting = false; message("Cannot start guest: " + error); }
    }
    void importGuest(Uri uri) {
        if (running() || starting || hasImage()) return;
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
    private void releaseForeground() { stopForeground(true); foreground = false; stopSelf(); }
    private void boot() {
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
        if (imageWorker != null) imageWorker.interrupt();
        if (guest != null) guest.destroyForcibly();
        unregisterReceiver(receiver);
        receiverThread.quitSafely();
        listeners.clear();
        super.onDestroy();
    }
}
