// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pockethostnativeprobe;

import android.app.Activity;
import android.content.pm.ApplicationInfo;
import android.os.Bundle;
import android.util.Base64;
import java.io.File;
import java.io.FileInputStream;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

/** Runs the packaged legacy tracer in an ordinary host UID, without a guest image. */
public final class MainActivity extends Activity {
    private volatile String report = "{}";
    private static void check(boolean value, String message) throws Exception {
        if (!value) throw new Exception(message);
    }
    private static String text(File file) throws Exception {
        check(file.length() < 65536, "Probe output exceeded limit");
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
    }
    private static String sha(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (FileInputStream in = new FileInputStream(file)) {
            byte[] bytes = new byte[65536]; int n;
            while ((n = in.read(bytes)) != -1) digest.update(bytes, 0, n);
        }
        StringBuilder out = new StringBuilder();
        for (byte b : digest.digest()) out.append(String.format("%02x", b & 255));
        return out.toString();
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        new Thread(this::probe, "PocketHostNativeProbe").start();
    }
    private void probe() {
        JSONObject result = new JSONObject();
        java.lang.Process process = null;
        try {
            int uid = android.os.Process.myUid(), pid = android.os.Process.myPid();
            check(uid >= 10000, "Host UID is privileged");
            check((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) == 0, "App is debuggable");
            String nonce = UUID.randomUUID().toString().replace("-", "");
            File dir = new File(getFilesDir(), nonce), root = new File(dir, "root"), tmp = new File(dir, "tmp");
            check(root.mkdirs() && tmp.mkdir(), "Cannot create unique test directory");
            File base = new File(dir, "base"), nested = new File(dir, "nested"), override = new File(dir, "override");
            check(base.mkdir() && nested.mkdir(), "Cannot create binding fixtures");
            Files.write(new File(base, "token").toPath(), nonce.getBytes(StandardCharsets.US_ASCII));
            Files.write(new File(nested, "token").toPath(), "wrong-parent-value".getBytes(StandardCharsets.US_ASCII));
            Files.write(override.toPath(), ("nested-" + nonce).getBytes(StandardCharsets.US_ASCII));
            File binary = new File(getApplicationInfo().nativeLibraryDir, "libproot.so");
            result.put("hostUid", uid).put("hostPid", pid).put("nonce", nonce).put("prootSha256", sha(binary));
            result.put("debuggable", false).put("hostStatusBefore", text(new File("/proc/self/status")));
            List<String> command = new ArrayList<>(Arrays.asList(binary.getPath(), "--kill-on-exit", "-0", "-r", root.getPath(), "-w", "/"));
            for (String path : new String[]{"/system", "/apex", "/linkerconfig", "/dev", "/proc"}) {
                if (new File(path).exists()) command.addAll(Arrays.asList("-b", path));
            }
            command.addAll(Arrays.asList("-b", base.getPath() + ":/pocket-probe", "-b", nested.getPath() + ":/pocket-probe/nested", "-b", override.getPath() + ":/pocket-probe/nested/token"));
            String shell = "set -e; test \"$(id -u)\" = 0; test \"$(cat /pocket-probe/token)\" = \"$POCKET_TEST_NONCE\"; "
                + "test \"$(cat /pocket-probe/nested/token)\" = \"nested-$POCKET_TEST_NONCE\"; "
                + "/system/bin/sh -c 'test \"$(id -u)\" = 0 && test \"$(cat /pocket-probe/nested/token)\" = \"nested-$POCKET_TEST_NONCE\"'; "
                + "printf 'POCKET_PROOT_TRACE_STATUS\\n'; cat /proc/self/status; "
                + "printf 'POCKET_PROOT_BIND_EXEC_PASS nonce=%s\\n' \"$POCKET_TEST_NONCE\"";
            command.addAll(Arrays.asList("/system/bin/sh", "-c", shell));
            ProcessBuilder builder = new ProcessBuilder(command);
            builder.environment().clear();
            builder.environment().put("PATH", "/system/bin");
            builder.environment().put("PROOT_NO_SECCOMP", "1");
            builder.environment().put("POCKET_PARENT_PID", Integer.toString(pid));
            builder.environment().put("PROOT_TMP_DIR", tmp.getPath());
            builder.environment().put("POCKET_TEST_NONCE", nonce);
            File log = new File(dir, "trace.log"); builder.redirectErrorStream(true).redirectOutput(log);
            process = builder.start();
            check(process.waitFor(30, TimeUnit.SECONDS), "PRoot binding/exec timed out");
            String output = text(log);
            result.put("traceOutput", output).put("traceExitCode", process.exitValue());
            check(process.exitValue() == 0 && output.contains("POCKET_PROOT_BIND_EXEC_PASS nonce=" + nonce), "PRoot binding/exec failed");
            check(text(new File(base, "token")).equals(nonce) && text(override).equals("nested-" + nonce), "Binding source changed");
            builder.environment().put("POCKET_PARENT_PID", "1");
            File denied = new File(dir, "denied.log"); builder.redirectOutput(denied);
            process = builder.start();
            check(process.waitFor(10, TimeUnit.SECONDS), "Parent-mismatch check timed out");
            result.put("wrongParentExitCode", process.exitValue()).put("wrongParentOutput", text(denied));
            check(process.exitValue() != 0 && text(denied).contains("Pocket tracer parent disappeared or death signal setup failed"), "Incorrect parent accepted");
            result.put("hostStatusAfter", text(new File("/proc/self/status"))).put("validation", "PASS");
        } catch (Throwable error) {
            try { result.put("validation", "FAIL").put("error", error.toString()); } catch (Exception ignored) { }
        } finally {
            if (process != null && process.isAlive()) process.destroyForcibly();
            report = result.toString();
        }
    }
    @Override public void dump(String prefix, java.io.FileDescriptor fd, PrintWriter writer, String[] args) {
        writer.println("POCKET_HOST_NATIVE=" + Base64.encodeToString(report.getBytes(StandardCharsets.UTF_8), Base64.NO_WRAP));
    }
}
