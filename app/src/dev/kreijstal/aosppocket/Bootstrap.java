// SPDX-License-Identifier: Apache-2.0
package dev.kreijstal.aosppocket;

import android.content.Context;
import android.os.Build;
import android.os.Process;
import android.os.SharedMemory;
import android.util.AtomicFile;
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;
import org.json.*;

final class Bootstrap {
    static final String REPORT = "bootstrap-result.json";

    static JSONObject run(Context context) throws Exception {
        long started = android.os.SystemClock.elapsedRealtime();
        File root = new File(context.getFilesDir(), "instances/default/rootfs");
        File data = new File(root, "data/pocket");
        if (!data.isDirectory() && !data.mkdirs()) throw new IOException("Cannot create instance data");
        if (!new File(root, "system/etc").isDirectory()
                && !new File(root, "system/etc").mkdirs()) throw new IOException("Cannot create guest etc");
        File hello = new File(data, "boot.txt");
        try (FileOutputStream out = new FileOutputStream(hello)) {
            out.write("AOSP Pocket native bootstrap\n".getBytes(StandardCharsets.UTF_8));
        }
        File nativeDir = new File(context.getApplicationInfo().nativeLibraryDir);
        File toybox = new File(nativeDir, "libtoybox_exec.so");
        File probe = new File(nativeDir, "libpocket_probe.so");
        JSONObject report = new JSONObject();
        report.put("stage", "native_bootstrap");
        report.put("android_framework_booted", false);
        report.put("guest_isolation_verified", false);
        report.put("uses_host_kernel_and_bionic", true);
        report.put("host_sdk", Build.VERSION.SDK_INT);
        report.put("host_abis", new JSONArray(Arrays.asList(Build.SUPPORTED_ABIS)));
        report.put("app_uid", Process.myUid());
        report.put("rootfs", root.getAbsolutePath());
        report.put("executable_directory", nativeDir.getAbsolutePath());
        report.put("source", new JSONObject(read(context.getAssets().open("sources.lock.json"), 16384)));
        JSONArray steps = new JSONArray();
        report.put("steps", steps);
        JSONObject nativeResult = command(root, probe.getAbsolutePath());
        steps.put(nativeResult);
        boolean okay = nativeResult.optBoolean("ok");
        if (okay) {
            JSONObject capabilities = new JSONObject(nativeResult.getString("stdout"));
            report.put("capabilities", capabilities);
            boolean uidMatches = capabilities.getInt("uid") == Process.myUid()
                    && capabilities.getInt("euid") == Process.myUid();
            report.put("native_uid_matches_app", uidMatches);
            okay &= uidMatches && Process.myUid() >= 10000;
        }
        String[][] commands = {{"echo", "aosp-pocket-ready"}, {"uname", "-m"},
                {"id", "-u"}, {"pwd"}, {"cat", "data/pocket/boot.txt"}};
        String[] expected = {"aosp-pocket-ready", "aarch64", Integer.toString(Process.myUid()),
                root.getCanonicalPath(), "AOSP Pocket native bootstrap"};
        for (int i = 0; i < commands.length; i++) {
            String[] argv = new String[commands[i].length + 3];
            argv[0] = probe.getAbsolutePath();
            argv[1] = "exec-toybox";
            argv[2] = toybox.getAbsolutePath();
            System.arraycopy(commands[i], 0, argv, 3, commands[i].length);
            JSONObject result = command(root, argv);
            result.put("expected_stdout", expected[i]);
            boolean matches = result.optBoolean("ok")
                    && result.optString("stdout").trim().equals(expected[i]);
            result.put("output_matches", matches);
            okay &= matches;
            steps.put(result);
        }
        try (SharedMemory memory = SharedMemory.create("pocket-bootstrap", 4096)) {
            ByteBuffer mapped = memory.mapReadWrite();
            try {
                mapped.putInt(0, 0x504f434b);
                report.put("android_shared_memory", mapped.getInt(0) == 0x504f434b);
            } finally { SharedMemory.unmap(mapped); }
        } catch (Exception e) {
            report.put("android_shared_memory", false);
            report.put("shared_memory_error", e.toString());
        }
        report.put("bootstrap_ok", okay);
        report.put("elapsed_ms", android.os.SystemClock.elapsedRealtime() - started);
        save(context, report);
        return report;
    }

    private static JSONObject command(File cwd, String... argv) throws Exception {
        long started = android.os.SystemClock.elapsedRealtime();
        JSONObject result = new JSONObject().put("argv", new JSONArray(Arrays.asList(argv)));
        java.lang.Process process = null;
        ExecutorService readers = Executors.newSingleThreadExecutor();
        try {
            ProcessBuilder builder = new ProcessBuilder(argv).directory(cwd).redirectErrorStream(true);
            // Keep the host environment honest. No LD_PRELOAD, identity spoofing,
            // namespace tricks, shell UID helper or external ADB authorization.
            builder.environment().clear();
            builder.environment().put("LANG", "C");
            builder.environment().put("HOME", new File(cwd, "data/pocket").getAbsolutePath());
            process = builder.start();
            final InputStream stream = process.getInputStream();
            Future<String> output = readers.submit(new Callable<String>() {
                @Override public String call() throws Exception { return read(stream, 32768); }
            });
            if (!process.waitFor(5, TimeUnit.SECONDS)) {
                process.destroyForcibly();
                throw new IOException("Native executable exceeded 5 second deadline");
            }
            result.put("stdout", output.get(2, TimeUnit.SECONDS));
            result.put("exit_code", process.exitValue());
            result.put("ok", process.exitValue() == 0);
        } catch (Exception e) {
            result.put("ok", false);
            result.put("error", e.toString());
        } finally {
            if (process != null) {
                process.destroyForcibly();
                process.getInputStream().close();
                process.getErrorStream().close();
                process.getOutputStream().close();
            }
            readers.shutdownNow();
        }
        result.put("elapsed_ms", android.os.SystemClock.elapsedRealtime() - started);
        return result;
    }

    static String read(InputStream input, int limit) throws IOException {
        try (InputStream in = input; ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buf = new byte[2048];
            int n;
            while ((n = in.read(buf)) != -1) {
                if (out.size() + n > limit) throw new IOException("Output exceeded " + limit + " bytes");
                out.write(buf, 0, n);
            }
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        }
    }

    static void save(Context context, JSONObject report) throws IOException, JSONException {
        AtomicFile target = new AtomicFile(new File(context.getFilesDir(), REPORT));
        FileOutputStream stream = target.startWrite();
        try {
            stream.write(report.toString(2).getBytes(StandardCharsets.UTF_8));
            target.finishWrite(stream);
        } catch (Exception e) {
            target.failWrite(stream);
            throw new IOException("Cannot save report", e);
        }
    }
}
