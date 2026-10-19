// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.app.Activity;
import android.content.*;
import android.os.*;
import android.system.Os;
import android.widget.TextView;
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.util.zip.*;
import org.json.JSONObject;

/** Isolated development APK: real Android 9 libraries, no full guest boot. */
public class Api28ProbeActivity extends Activity {
    private final GuestBinder receiver = new GuestBinder();
    private volatile java.lang.Process manager, client, propertyServer, propertyWaiter, initProcess;
    private volatile boolean closed;
    private HandlerThread receiverThread;
    private int extractedFiles, cachedFiles;
    private TextView status;

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        status = new TextView(this);
        status.setTextSize(18);
        status.setPadding(24, 48, 24, 24);
        setContentView(status);
        status.setText("Android 9 / API 28 init integration\nPreparing independent test…");
        receiverThread = new HandlerThread("Api28Binder");
        receiverThread.start();
        IntentFilter filter = new IntentFilter(GuestBinder.ACTION);
        Handler handler = new Handler(receiverThread.getLooper());
        if (Build.VERSION.SDK_INT >= 33)
            registerReceiver(receiver, filter, null, handler, Context.RECEIVER_NOT_EXPORTED);
        else registerReceiver(receiver, filter, null, handler);
        new Thread(this::probe, "Api28Probe").start();
    }

    protected void message(String value) { runOnUiThread(() -> status.setText(value)); }

    protected File extract() throws Exception {
        File root = new File(getFilesDir(), "api28-rootfs");
        root.mkdirs();
        SharedPreferences cache = getSharedPreferences("api28-fixture-cache", MODE_PRIVATE);
        SharedPreferences.Editor stamps = cache.edit();
        try (ZipFile apk = new ZipFile(getApplicationInfo().sourceDir)) {
            java.util.Enumeration<? extends ZipEntry> entries = apk.entries();
            final String prefix = "assets/api28-rootfs/";
            while (entries.hasMoreElements()) {
                ZipEntry entry = entries.nextElement();
                if (!entry.getName().startsWith(prefix)) continue;
                String name = entry.getName().substring(prefix.length());
                if (!name.endsWith(".payload")) throw new IOException("Invalid probe payload");
                name = name.substring(0, name.length() - 8);
                File file = new File(root, name);
                if (!file.getCanonicalPath().startsWith(root.getCanonicalPath() + "/"))
                    throw new IOException("Invalid probe path");
                String stamp = entry.getCrc() + ":" + entry.getSize();
                if (stamp.equals(cache.getString(name, "")) && file.isFile() && file.length() == entry.getSize()) {
                    Os.chmod(file.getPath(), 0755);
                    ++cachedFiles;
                    continue;
                }
                file.getParentFile().mkdirs();
                try (InputStream in = apk.getInputStream(entry); OutputStream out = new FileOutputStream(file)) {
                    byte[] buffer = new byte[65536];
                    for (int n; (n = in.read(buffer)) != -1;) out.write(buffer, 0, n);
                }
                Os.chmod(file.getPath(), 0755);
                stamps.putString(name, stamp);
                ++extractedFiles;
            }
        }
        if (extractedFiles > 0) stamps.apply();
        new File(root, "tmp").mkdirs();
        // PRoot's old bind-only probe created a mode-000 /dev placeholder.
        // The rootless guest now needs actual writable private subdirectories.
        File dev = new File(root, "dev");
        if (!dev.isDirectory() && !dev.mkdir()) throw new IOException("Cannot create guest /dev");
        Os.chmod(dev.getPath(), 0755);
        new File(root, "dev/socket").mkdirs();
        resetProperties(root);
        return root;
    }

    private void resetProperties(File root) throws IOException {
        File area = new File(root, "dev/__properties__");
        area.mkdirs();
        File[] entries = area.listFiles();
        if (entries == null) throw new IOException("Cannot access guest property area");
        for (File entry : entries)
            if (!entry.delete()) throw new IOException("Cannot remove prior guest property file " + entry);
    }

    private void hostTemplates(File root) throws Exception {
        GuestBinder.prepare(this, root);
        Files.write(new File(root, "host-api").toPath(), Integer.toString(Build.VERSION.SDK_INT).getBytes("UTF-8"));
        Parcel p = Parcel.obtain();
        try {
            // No Binder objects, so marshall() is safe for this one template.
            p.writeInterfaceToken("android.os.IServiceManager");
            p.writeString("activity");
            byte[] data = p.marshall();
            try (OutputStream out = new FileOutputStream(new File(root, "activityServiceRequest"))) {
                out.write(ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
                    .putLong(data.length).putLong(0).array());
                out.write(data);
            }
            p.setDataSize(0);
            p.setDataPosition(0);
            String descriptor = "com.github.ananbox.ILocalInterface";
            p.writeInterfaceToken(descriptor);
            int stringSize = 4 + ((2 * (descriptor.length() + 1) + 3) & ~3);
            int stringPosition = p.dataSize() - stringSize;
            if (stringPosition < 0) throw new IOException("Unsupported host interface header");
            Files.write(new File(root, "host-token-position").toPath(),
                Integer.toString(stringPosition).getBytes("UTF-8"));
        } finally { p.recycle(); }
    }

    private java.lang.Process start(File root, String mode, boolean virtual) throws IOException {
        return startProgram(root, "pocket-api28-probe",
            new String[]{virtual && !mode.equals("manager") ? "client-virtual" : mode},
            virtual ? (mode.equals("manager") ? "0:0" : "39999:39999") : null, mode);
    }

    protected java.lang.Process startProgram(File root, String binary, String[] args, String identity, String output) throws IOException {
        if (closed) throw new IOException("Probe closed");
        File tmp = new File(getFilesDir(), "tmp");
        tmp.mkdirs();
        java.util.List<String> command = new java.util.ArrayList<>(java.util.Arrays.asList(
            getApplicationInfo().nativeLibraryDir + "/libproot.so", "--kill-on-exit",
            "-r", root.getPath(), "-w", "/", "-b", "/dev", "-b", "/proc",
            "-b", new File(root, "dev/__properties__").getPath() + ":/dev/__properties__",
            "-b", new File(root, "dev/socket").getPath() + ":/dev/socket"));
        if (output.startsWith("zygote")) {
            // A separate, initially cache-free /data for the read-only image
            // zygote profile. ART treats /data as its global cache namespace.
            command.add("-b");
            command.add(new File(root, "zygote-data").getPath() + ":/data");
            command.add("-b");
            command.add(new File(getFilesDir(), output + ".log").getPath() + ":/pocket-console");
        }
        // Even a real-UID client needs guest metadata translation: bionic
        // requires property files to appear root-owned when mapping them.
        command.add("-i");
        command.add(identity != null ? identity : android.os.Process.myUid() + ":" + android.os.Process.myUid());
        command.add("/system/bin/" + binary);
        java.util.Collections.addAll(command, args);
        ProcessBuilder builder = new ProcessBuilder(command);
        builder.environment().put("PROOT_NO_SECCOMP", "1");
        builder.environment().put("PROOT_TMP_DIR", tmp.getPath());
        builder.environment().put("POCKET_PARENT_PID", Integer.toString(android.os.Process.myPid()));
        builder.environment().put("TMPDIR", "/tmp");
        if (binary.equals("dalvikvm64") || output.equals("zygote-init")) builder.environment().put("POCKET_DEBUG_SIGNALS", "1");
        builder.environment().put("ANDROID_ROOT", "/system");
        builder.environment().put("ANDROID_DATA", "/data");
        for (String name : new String[]{"BOOTCLASSPATH", "DEX2OATBOOTCLASSPATH", "SYSTEMSERVERCLASSPATH",
                "ANDROID_ART_ROOT", "ANDROID_RUNTIME_ROOT", "ANDROID_I18N_ROOT", "ANDROID_TZDATA_ROOT"})
            builder.environment().remove(name);
        builder.environment().remove("LD_LIBRARY_PATH");
        builder.environment().remove("LD_PRELOAD");
        // These supervised programs have no interactive stdin; match init's
        // /dev/null input instead of leaving a Java ProcessBuilder pipe open.
        builder.redirectInput(new File("/dev/null"));
        builder.redirectErrorStream(true);
        File logFile = programLog(output);
        try (FileOutputStream clear = new FileOutputStream(logFile)) { }
        // Forked zygote children reopen Java's duplicated stdout/err handles.
        // Append mode keeps those descriptions from overwriting each other.
        builder.redirectOutput(ProcessBuilder.Redirect.appendTo(logFile));
        return builder.start();
    }

    protected File programLog(String name) { return new File(getFilesDir(), name + ".log"); }

    private String log(String name) throws IOException {
        return new String(Files.readAllBytes(programLog(name).toPath()), "UTF-8");
    }

    protected boolean extendedStage(File root, JSONObject result) throws Exception { return false; }

    private void probe() {
        JSONObject result = new JSONObject();
        try {
            result.put("guest_sdk", 28).put("host_sdk", Build.VERSION.SDK_INT)
                .put("host_uid", android.os.Process.myUid()).put("full_guest_boot_verified", false);
            File root = extract();
            result.put("payload_files_extracted", extractedFiles).put("payload_files_cached", cachedFiles);
            hostTemplates(root);
            if (extendedStage(root, result)) return;
            if ("zygote".equals(getIntent().getStringExtra("stage"))) {
                result.put("scope", "zygote_only");
                zygote(root, result);
                result.put("passed", true);
                message("PASS — Android 9 zygote fork, Java child, Binder and shutdown");
                return;
            }
            startProperties(root);
            propertyCheck(root, "fresh");
            propertyWaiter = startProgram(root, "pocket-property-probe-api28", new String[]{"wait"}, "39999:39999", "property-wait");
            waitForMarker(propertyWaiter, "property-wait", "API28_PROPERTY_WAIT_READY");
            propertyCheck(root, "write");
            if (!propertyWaiter.waitFor(15, java.util.concurrent.TimeUnit.SECONDS) || propertyWaiter.exitValue() != 0)
                throw new IOException("Guest property wait failed:\n" + log("property-wait"));
            result.put("properties_passed", true).put("property_check_log", log("property-fresh"))
                .put("property_wait_log", log("property-wait"));
            String output = round(root, false);
            result.put("ordinary_uid_passed", true).put("client_log", output).put("manager_log", log("manager"));
            output = round(root, true);
            result.put("virtual_uid_passed", true).put("virtual_client_log", output).put("virtual_manager_log", log("manager"));
            art(root, result);
            stopProperties(root);
            result.put("property_server_log", log("properties"));
            resetProperties(root);
            startProperties(root);
            propertyCheck(root, "fresh");
            stopProperties(root);
            result.put("property_restart_passed", true).put("property_restart_log", log("properties"));
            initIntegration(root, result);
            zygote(root, result);
            result.put("passed", true);
            message("PASS — Android 9 init integration\n\nPrivate properties, update notifications, "
                + "restart, Binder and FD transfer passed.\n\nART: threads, reflection, exceptions "
                + "and native child execution passed.\n\nAOSP init actions, sockets, child reaping, "
                + "restart and shutdown passed. Zygote fork/Java/Binder passed. Launcher is not tested.");
        } catch (Exception error) {
            try { result.put("passed", false).put("error", error.toString()); } catch (Exception ignored) { }
            message("API 28 bring-up failed:\n" + error);
        } finally {
            if (initProcess != null && initProcess.isAlive()) initProcess.destroyForcibly();
            if (client != null && client.isAlive()) client.destroyForcibly();
            if (manager != null && manager.isAlive()) manager.destroyForcibly();
            if (propertyWaiter != null && propertyWaiter.isAlive()) propertyWaiter.destroyForcibly();
            if (propertyServer != null && propertyServer.isAlive()) propertyServer.destroyForcibly();
            try { Files.write(new File(getFilesDir(), "api28-result.json").toPath(), result.toString(2).getBytes("UTF-8")); }
            catch (Exception ignored) { }
        }
    }

    private void initIntegration(File root, JSONObject result) throws Exception {
        message("Android 9: testing AOSP init parsing, actions and real child services…");
        resetProperties(root);
        initProcess = startProgram(root, "pocket-init-api28", new String[]{"/system/etc/init/init.invalid.rc"}, "0:0", "init-invalid");
        if (!initProcess.waitFor(15, java.util.concurrent.TimeUnit.SECONDS) || initProcess.exitValue() != 66 ||
                !log("init-invalid").contains("INIT_CONFIG_REJECTED") || log("init-invalid").contains("INIT_CHILD"))
            throw new IOException("Init accepted unsupported imported configuration:\n" + log("init-invalid"));
        result.put("init_invalid_config_log", log("init-invalid"));
        resetProperties(root);
        initProcess = startProgram(root, "pocket-init-api28", new String[]{"/system/etc/init/init.pocket.rc"}, "0:0", "init");
        waitForMarker(initProcess, "init", "API28_INIT_READY");
        client = startProgram(root, "pocket-init-test-api28", new String[]{"client"}, "39999:39999", "init-client");
        if (!client.waitFor(90, java.util.concurrent.TimeUnit.SECONDS) || client.exitValue() != 0 ||
                !log("init-client").contains("API28_INIT_CLIENT_PASS"))
            throw new IOException("Init lifecycle test failed:\n" + log("init-client") + "\n" + log("init"));
        propertyCheck(root, "stop");
        if (!initProcess.waitFor(15, java.util.concurrent.TimeUnit.SECONDS) || initProcess.exitValue() != 0 ||
                !log("init").contains("remaining=0 failed=0"))
            throw new IOException("Init did not shut down and reap children:\n" + log("init"));
        client = startProgram(root, "pocket-init-test-api28", new String[]{"stopped"}, "39999:39999", "init-stopped");
        if (!client.waitFor(15, java.util.concurrent.TimeUnit.SECONDS) || client.exitValue() != 0)
            throw new IOException("Init left service state or socket behind:\n" + log("init-stopped"));
        result.put("init_passed", true).put("init_log", log("init"))
            .put("init_client_log", log("init-client")).put("init_shutdown_log", log("init-stopped"));
    }

    private void zygote(File root, JSONObject result) throws Exception {
        message("Android 9: starting the real AOSP zygote and testing Java process creation…");
        receiver.reset();
        new File(root, "zygote-data").mkdirs();
        resetProperties(root);
        File childReport = new File(root, "zygote-data/pocket-zygote-child.json");
        if (childReport.exists() && !childReport.delete()) throw new IOException("Cannot remove prior zygote report");
        String nonce = java.util.UUID.randomUUID().toString().replace("-", "").substring(0, 16);
        initProcess = startProgram(root, "pocket-init-api28", new String[]{"/system/etc/init/init.zygote.rc"}, "0:0", "zygote-init");
        waitForMarker(initProcess, "zygote-init", "API28_INIT_READY");
        client = startProgram(root, "pocket-zygote-client-api28", new String[]{"fork", nonce}, "39999:39999", "zygote-client");
        if (!client.waitFor(120, java.util.concurrent.TimeUnit.SECONDS) || client.exitValue() != 0)
            throw new IOException("Zygote protocol/fork failed:\n" + log("zygote-client") + "\n" + log("zygote-init"));
        JSONObject child = new JSONObject(new String(Files.readAllBytes(childReport.toPath()), "UTF-8"));
        if (!child.getBoolean("passed") || child.getInt("uid") != 39999 || child.getInt("sdk") != 28 || !nonce.equals(child.getString("nonce")))
            throw new IOException("Bad/stale zygote child report: " + child);
        int pid = child.getInt("pid");
        java.util.regex.Matcher fork = java.util.regex.Pattern.compile("parent_pid=(\\d+) child_pid=(\\d+) peer_kernel_uid=(\\d+)").matcher(log("zygote-client"));
        if (!fork.find() || pid != Integer.parseInt(fork.group(2)) || android.os.Process.myUid() != Integer.parseInt(fork.group(3)))
            throw new IOException("Zygote PID/peer identity mismatch");
        String proc = new String(Files.readAllBytes(new File("/proc/" + pid + "/status").toPath()), "UTF-8");
        java.util.regex.Matcher ids = java.util.regex.Pattern.compile("(?m)^Uid:\\s+(\\d+)").matcher(proc);
        java.util.regex.Matcher parent = java.util.regex.Pattern.compile("(?m)^PPid:\\s+(\\d+)").matcher(proc);
        if (!ids.find() || Integer.parseInt(ids.group(1)) != android.os.Process.myUid() || !parent.find() || !parent.group(1).equals(fork.group(1)))
            throw new IOException("Zygote child escaped host UID or has the wrong parent");
        String context = new String(Files.readAllBytes(new File("/proc/" + pid + "/attr/current").toPath()), "UTF-8").replace("\u0000", "").trim();
        if (!context.contains("untrusted_app")) throw new IOException("Unexpected zygote child SELinux context " + context);
        result.put("zygote_child_selinux", context);
        result.put("zygote_child", child).put("zygote_child_status", proc).put("zygote_client_log", log("zygote-client"));
        // AOSP ZygoteConnection moves the child to its client's POSIX group.
        // Until descendant ownership is implemented, explicitly terminate the
        // verified test child while zygote is alive to reap it.
        Os.kill(pid, 15);
        for (int n = 0; n < 100 && new File("/proc/" + pid).exists(); ++n) Thread.sleep(50);
        if (new File("/proc/" + pid).exists()) throw new IOException("Zygote did not reap its test child");
        client = startProgram(root, "pocket-zygote-client-api28", new String[]{"query"}, "39999:39999", "zygote-query");
        if (!client.waitFor(15, java.util.concurrent.TimeUnit.SECONDS) || client.exitValue() != 0)
            throw new IOException("Zygote did not survive child exit:\n" + log("zygote-query"));
        propertyCheck(root, "stop");
        if (!initProcess.waitFor(15, java.util.concurrent.TimeUnit.SECONDS) || initProcess.exitValue() != 0 ||
                !log("zygote-init").contains("remaining=0 failed=0"))
            throw new IOException("Zygote init did not shut down:\n" + log("zygote-init"));
        if (new File(root, "zygote-data/dalvik-cache").exists())
            throw new IOException("Image-only zygote profile unexpectedly created a compiler cache");
        if (!log("zygote-init").contains("API28_LOG_WRITER_TEST"))
            throw new IOException("Guest log writer did not capture the real liblog test message");
        result.put("zygote_image_only_cache_absent", true);
        result.put("zygote_passed", true).put("zygote_init_log", log("zygote-init")).put("zygote_query_after_child_log", log("zygote-query"));
    }

    private void art(File root, JSONObject result) throws Exception {
        message("Android 9: testing ART, Java threads and child execution…");
        new File(root, "data/dalvik-cache/arm64").mkdirs();
        ByteArrayOutputStream contents = new ByteArrayOutputStream();
        try (InputStream in = getAssets().open("bootclasspath.txt")) {
            byte[] buffer = new byte[4096];
            for (int count; (count = in.read(buffer)) != -1;) contents.write(buffer, 0, count);
        }
        String bootclasspath = contents.toString("UTF-8").trim();
        client = startProgram(root, "dalvikvm64", new String[]{"-Xint", "-Xnorelocate", "-Xnoimage-dex2oat",
            "-Ximage:/system/framework/boot.art", "-Xbootclasspath:" + bootclasspath,
            "-Xms16m", "-Xmx128m", "-cp", "/data/pocket-art-probe.dex",
            "dev.kreijstal.aosppocket.guest.ArtProbe"}, "39999:39999", "art");
        boolean finished = client.waitFor(120, java.util.concurrent.TimeUnit.SECONDS);
        if (!finished) client.destroyForcibly();
        String output = log("art");
        result.put("art_log", output).put("art_passed", finished && client.exitValue() == 0 && output.contains("API28_ART_PASS"));
        if (!result.getBoolean("art_passed")) throw new IOException("Android 9 ART test failed:\n" + output);
    }

    private void waitForMarker(java.lang.Process process, String name, String marker) throws Exception {
        for (int i = 0; i < 150 && !closed && process.isAlive(); ++i) {
            if (log(name).contains(marker)) return;
            Thread.sleep(200);
        }
        throw new IOException("Guest " + name + " did not become ready:\n" + log(name));
    }

    private void startProperties(File root) throws Exception {
        message("Android 9: starting private property service…");
        propertyServer = startProgram(root, "pocket-property-service-api28", new String[0], "0:0", "properties");
        waitForMarker(propertyServer, "properties", "API28_PROPERTIES_READY");
    }

    private void propertyCheck(File root, String mode) throws Exception {
        client = startProgram(root, "pocket-property-probe-api28", new String[]{mode}, "39999:39999", "property-" + mode);
        if (!client.waitFor(15, java.util.concurrent.TimeUnit.SECONDS) || client.exitValue() != 0)
            throw new IOException("Guest property " + mode + " failed:\n" + log("property-" + mode));
    }

    private void stopProperties(File root) throws Exception {
        propertyCheck(root, "stop");
        if (!propertyServer.waitFor(10, java.util.concurrent.TimeUnit.SECONDS) || propertyServer.exitValue() != 0 ||
                !log("properties").contains("API28_PROPERTIES_STOPPED"))
            throw new IOException("Guest property service did not shut down cleanly");
    }

    private String round(File root, boolean virtual) throws Exception {
        receiver.reset();
        try {
            manager = start(root, "manager", virtual);
            boolean ready = false;
            for (int i = 0; i < 150 && !closed && manager.isAlive(); ++i) {
                if (log("manager").contains("API28_READY")) { ready = true; break; }
                Thread.sleep(200);
            }
            if (!ready) throw new IOException("API 28 manager did not become ready:\n" + log("manager"));
            message("Android 9 Binder manager ready. Testing client and FD transfer…");
            client = start(root, "client", virtual);
            if (!client.waitFor(30, java.util.concurrent.TimeUnit.SECONDS)) throw new IOException("API 28 client timed out");
            String output = log("client");
            boolean passed = client.exitValue() == 0 && output.contains("API28_RESULT status=0 answer=28");
            if (!passed) throw new IOException("API 28 client failed (" + client.exitValue() + "):\n" + output);
            return output;
        } finally {
            if (client != null && client.isAlive()) client.destroyForcibly();
            if (manager != null && manager.isAlive()) manager.destroyForcibly();
            if (manager != null) manager.waitFor(5, java.util.concurrent.TimeUnit.SECONDS);
        }
    }

    @Override public void onDestroy() {
        closed = true;
        if (initProcess != null && initProcess.isAlive()) initProcess.destroyForcibly();
        if (client != null && client.isAlive()) client.destroyForcibly();
        if (manager != null && manager.isAlive()) manager.destroyForcibly();
        if (propertyWaiter != null && propertyWaiter.isAlive()) propertyWaiter.destroyForcibly();
        if (propertyServer != null && propertyServer.isAlive()) propertyServer.destroyForcibly();
        unregisterReceiver(receiver);
        receiverThread.quitSafely();
        super.onDestroy();
    }
}
