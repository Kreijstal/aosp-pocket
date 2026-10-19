// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.direct;

import android.app.Activity;
import android.os.Bundle;
import android.os.Process;
import android.system.Os;
import android.widget.TextView;
import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.TimeUnit;
import java.util.regex.*;
import java.util.zip.*;
import org.json.*;

/** Separate development app: controlled native guest ABI, no full Android boot. */
public final class DirectProbeActivity extends Activity {
    private volatile java.lang.Process running;
    private volatile boolean closed;
    private TextView status;
    private String nonce;

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        status = new TextView(this); status.setTextSize(17); status.setPadding(24, 40, 24, 24);
        setContentView(status);
        nonce = getIntent().getStringExtra("nonce");
        if (nonce == null) nonce = UUID.randomUUID().toString();
        new Thread(this::test, "DirectGuestProbe").start();
    }
    private void message(String text) { runOnUiThread(() -> status.setText(text)); }
    private String read(File file) throws IOException { return new String(Files.readAllBytes(file.toPath()), "UTF-8"); }
    private JSONObject abi() throws Exception {
        try (InputStream in = getAssets().open("direct-abi.json"); ByteArrayOutputStream bytes = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[4096];
            for (int n; (n = in.read(buffer)) != -1;) bytes.write(buffer, 0, n);
            return new JSONObject(bytes.toString("UTF-8"));
        }
    }
    private void write(File file, String text) throws IOException { Files.write(file.toPath(), text.getBytes("UTF-8")); }

    private File extract(String mode) throws Exception {
        File root = new File(getFilesDir(), mode + "-root"); root.mkdirs();
        try (ZipFile apk = new ZipFile(getApplicationInfo().sourceDir)) {
            Enumeration<? extends ZipEntry> entries = apk.entries();
            String prefix = "assets/direct-rootfs/";
            while (entries.hasMoreElements()) {
                ZipEntry entry = entries.nextElement();
                if (!entry.getName().startsWith(prefix)) continue;
                String name = entry.getName().substring(prefix.length());
                if (!name.endsWith(".payload")) throw new IOException("Invalid guest entry");
                File file = new File(root, name.substring(0, name.length() - 8));
                if (!file.getCanonicalPath().startsWith(root.getCanonicalPath() + "/")) throw new IOException("Invalid guest path");
                file.getParentFile().mkdirs();
                try (InputStream in = apk.getInputStream(entry); OutputStream out = new FileOutputStream(file)) {
                    byte[] bytes = new byte[65536];
                    for (int n; (n = in.read(bytes)) != -1;) out.write(bytes, 0, n);
                }
                Os.chmod(file.getPath(), 0755);
            }
        }
        new File(root, "data").mkdirs();
        new File(root, "proc").mkdirs();
        new File(root, "dev").mkdirs();
        Files.deleteIfExists(new File(root, "data/release").toPath());
        Files.deleteIfExists(new File(root, "data/dir/file").toPath());
        write(new File(root, "data/fixture"), "pocket-direct-fixture\n");
        File config = new File(root, "system/etc/ld.config.txt"); config.getParentFile().mkdirs();
        String prefix = mode.equals("direct") ? root.getPath() : "";
        write(config, "dir.pocket = " + (prefix.isEmpty() ? "/system" : prefix) + "\n[pocket]\n"
            + "namespace.default.isolated = true\nnamespace.default.search.paths = " + prefix + "/system/lib64\n"
            + "namespace.default.permitted.paths = " + (prefix.isEmpty() ? "/system" : prefix) + "\n");
        patchInterpreter(new File(root, "system/bin/pocket-direct-probe"),
            mode.equals("direct") ? new File(root, "system/bin/linker64").getPath() : "/system/bin/linker64");
        return root;
    }

    private void patchInterpreter(File executable, String interpreter) throws Exception {
        byte[] elf = Files.readAllBytes(executable.toPath());
        ByteBuffer b = ByteBuffer.wrap(elf).order(ByteOrder.LITTLE_ENDIAN);
        if (b.getInt(0) != 0x464c457f || elf[4] != 2 || elf[5] != 1) throw new IOException("Expected ELF64 LE");
        int table = Math.toIntExact(b.getLong(32)), size = b.getShort(54) & 65535, count = b.getShort(56) & 65535;
        boolean found = false;
        for (int n = 0; n < count; ++n) {
            int pos = table + n * size;
            if (b.getInt(pos) != 3) continue;
            int offset = Math.toIntExact(b.getLong(pos + 8)), capacity = Math.toIntExact(b.getLong(pos + 32));
            byte[] path = interpreter.getBytes("UTF-8");
            if (found || path.length + 1 > capacity || offset < 0 || capacity > elf.length - offset)
                throw new IOException("Invalid/reserved ELF interpreter space");
            Arrays.fill(elf, offset, offset + capacity, (byte)0);
            System.arraycopy(path, 0, elf, offset, path.length);
            found = true;
        }
        if (!found) throw new IOException("ELF interpreter missing");
        Files.write(executable.toPath(), elf);
    }

    private java.lang.Process start(File root, String mode, String label, boolean hold, String... args) throws Exception {
        if (closed) throw new IOException("Probe closed");
        List<String> command = new ArrayList<>();
        if (mode.equals("proot")) {
            Collections.addAll(command, getApplicationInfo().nativeLibraryDir + "/libproot.so", "--kill-on-exit",
                "-r", root.getPath(), "-b", "/proc", "-b", "/dev", "-w", "/", "-i", Process.myUid() + ":" + Process.myUid(),
                "/system/bin/pocket-direct-probe");
        } else command.add(new File(root, "system/bin/pocket-direct-probe").getPath());
        Collections.addAll(command, args);
        ProcessBuilder builder = new ProcessBuilder(command);
        builder.directory(root);
        Map<String, String> env = builder.environment();
        for (String name : new String[]{"LD_PRELOAD", "LD_LIBRARY_PATH", "LD_CONFIG_FILE", "POCKET_DIRECT_ROOT", "POCKET_DIRECT_LIBRARIES", "POCKET_DIRECT_HOLD"}) env.remove(name);
        env.put("LD_LIBRARY_PATH", mode.equals("proot") ? "/system/lib64" : new File(root, "system/lib64").getPath());
        env.put("LD_CONFIG_FILE", mode.equals("proot") ? "/system/etc/ld.config.txt" : new File(root, "system/etc/ld.config.txt").getPath());
        if (mode.equals("direct")) {
            env.put("POCKET_DIRECT_ROOT", root.getPath());
            env.put("POCKET_DIRECT_LIBRARIES", new File(root, "system/lib64").getPath());
        }
        if (mode.equals("proot")) {
            File tmp = new File(getFilesDir(), "proot-tmp"); tmp.mkdirs();
            env.put("PROOT_NO_SECCOMP", "1"); env.put("PROOT_TMP_DIR", tmp.getPath());
        }
        if (hold) env.put("POCKET_DIRECT_HOLD", "1");
        builder.redirectInput(new File("/dev/null"));
        builder.redirectErrorStream(true);
        builder.redirectOutput(new File(getFilesDir(), label + ".log"));
        running = builder.start(); return running;
    }
    private String finish(java.lang.Process p, String label) throws Exception {
        if (!p.waitFor(45, TimeUnit.SECONDS)) { p.destroyForcibly(); throw new IOException(label + " timeout"); }
        String log = read(new File(getFilesDir(), label + ".log"));
        if (p.exitValue() != 0) throw new IOException(label + " exited " + p.exitValue() + ":\n" + log);
        return log;
    }
    private JSONObject suite(File root, String mode) throws Exception {
        java.lang.Process p = start(root, mode, mode + "-suite", true, "suite", mode);
        File logFile = new File(getFilesDir(), mode + "-suite.log");
        Matcher ready = null;
        for (int n = 0; n < 600; ++n) {
            ready = Pattern.compile("DIRECT_READY mode=" + mode + " pid=(\\d+)").matcher(read(logFile));
            if (ready.find()) break;
            if (!p.isAlive()) { finish(p, mode + "-suite"); throw new IOException("Missing ready marker"); }
            Thread.sleep(50); ready = null;
        }
        if (ready == null) throw new IOException(mode + " did not become ready");
        int pid = Integer.parseInt(ready.group(1));
        String proc = read(new File("/proc/" + pid + "/status"));
        String maps = read(new File("/proc/" + pid + "/maps"));
        String context = read(new File("/proc/" + pid + "/attr/current")).replace("\u0000", "").trim();
        Matcher uid = Pattern.compile("(?m)^Uid:\\s+(\\d+)\\s+(\\d+)\\s+(\\d+)\\s+(\\d+)").matcher(proc);
        if (!uid.find()) throw new IOException("Missing kernel UID");
        for (int i = 1; i <= 4; ++i) if (Integer.parseInt(uid.group(i)) != Process.myUid()) throw new IOException("Wrong real UID");
        Matcher tracer = Pattern.compile("(?m)^TracerPid:\\s+(\\d+)").matcher(proc);
        if (!tracer.find() || (mode.equals("direct") ? Integer.parseInt(tracer.group(1)) != 0 : Integer.parseInt(tracer.group(1)) == 0))
            throw new IOException("Unexpected tracer for " + mode);
        if (!context.contains("untrusted_app") || !Pattern.compile("(?m)^Seccomp:\\s+2$").matcher(proc).find())
            throw new IOException("Host confinement changed");
        write(new File(root, "data/release"), "release\n");
        String log = finish(p, mode + "-suite");
        if (!log.contains("DIRECT_SUITE_PASS") || !log.contains("DIRECT_CHILD_PASS") ||
            !log.contains("DIRECT_FILES_PASS") || !log.contains("DIRECT_SOCKET_PASS") ||
            !log.contains(root.getPath() + "/system/lib64/libc.so") && mode.equals("direct"))
            throw new IOException("Suite/guest bionic verification failed: " + log);
        return new JSONObject().put("pid", pid).put("status", proc).put("selinux", context).put("maps", maps).put("log", log);
    }
    private long bench(File root, String mode, String label, String path) throws Exception {
        String log = finish(start(root, mode, label, false, "bench", path, "400"), label);
        Matcher value = Pattern.compile("DIRECT_BENCH iterations=400 elapsed_ns=(\\d+)").matcher(log);
        if (!value.find()) throw new IOException("Missing benchmark output: " + log);
        return Long.parseLong(value.group(1));
    }
    private void test() {
        JSONObject result = new JSONObject();
        try {
            result.put("nonce", nonce).put("host_uid", Process.myUid()).put("full_guest_boot_verified", false)
                .put("scope", "controlled_native_abi").put("third_party_syscall_interception", false).put("abi", abi());
            message("Preparing isolated Android 9 linker/bionic fixtures…");
            File direct = extract("direct"), proot = extract("proot");
            message("Testing stdio, metadata, Unix sockets and fork/exec without PRoot…");
            result.put("direct", suite(direct, "direct"));
            message("Checking the same guest program under PRoot…");
            result.put("proot", suite(proot, "proot"));
            JSONArray samples = new JSONArray();
            message("Measuring repeated open/read/close/stat operations…");
            for (int round = 0; round < 3; ++round) {
                JSONObject sample = new JSONObject().put("iterations", 400);
                // Alternate order; report operation-loop timing separately from startup.
                for (String mode : round % 2 == 0 ? new String[]{"baseline", "direct", "proot"} : new String[]{"proot", "direct", "baseline"}) {
                    File root = mode.equals("proot") ? proot : direct;
                    String path = mode.equals("baseline") ? new File(root, "data/fixture").getPath() : "/data/fixture";
                    sample.put(mode + "_ns", bench(root, mode, mode + "-bench-" + round, path));
                }
                samples.put(sample);
            }
            result.put("benchmark", samples).put("passed", true);
            message("PASS: guest files, stdio, directories, metadata, Unix sockets, FD transfer and fork/exec run without PRoot.\n\nAndroid 9 linker/bionic verified; host UID, SELinux and seccomp retained.\n\nThis is not a complete Android guest.");
        } catch (Exception e) {
            try { result.put("passed", false).put("error", e.toString()); } catch (Exception ignored) { }
            message("Direct backend failed:\n" + e);
        } finally {
            if (running != null && running.isAlive()) running.destroyForcibly();
            try { write(new File(getFilesDir(), "direct-result.json"), result.toString(2) + "\n"); }
            catch (Exception e) { message("Cannot save result: " + e); }
        }
    }
    @Override public void onDestroy() {
        closed = true;
        if (running != null && running.isAlive()) running.destroyForcibly();
        super.onDestroy();
    }
}
