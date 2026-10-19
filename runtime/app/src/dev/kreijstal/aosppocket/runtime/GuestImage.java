// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.system.Os;
import android.content.Context;
import java.io.*;
import java.util.*;
import java.util.zip.*;
import org.json.*;

/** Install a bundled or explicitly selected trusted AOSP Pocket image. */
final class GuestImage {
    private static final long LIMIT = 2L * 1024 * 1024 * 1024;
    private static final String BUNDLED = "assets/default-guest/";
    interface Progress { void update(long completed, long total); }
    static boolean hasBundled(Context context) {
        try (InputStream in = context.getAssets().open("default-guest/pocket-layout.json")) { return in.read() != -1; }
        catch (IOException error) { return false; }
    }
    static void installBundled(Context context, Progress progress) throws Exception {
        // Read compressed entries straight from the installed APK: no large
        // intermediate ZIP in the app's writable data directory.
        install(new File(context.getApplicationInfo().sourceDir), context.getFilesDir(), BUNDLED, progress);
    }
    static void prepareRuntime(Context context, File root) throws Exception {
        // Select by the guest ABI, never the host Android release. In
        // particular, do not inject the API 25 HostBinder fix into API 28.
        GuestProfile.detect(root).requireBootSupport();
        File library = child(root, "system/lib64/libpocket_binder_bootstrap.so");
        File temporary = new File(library.getParentFile(), "libpocket_binder_bootstrap.so.new");
        try (InputStream in = context.getAssets().open("guest-binder-bootstrap.so");
             OutputStream out = new FileOutputStream(temporary)) {
            copy(in, out, 1024 * 1024);
        }
        Os.chmod(temporary.getPath(), 0755);
        if (!temporary.renameTo(library)) throw new IOException("Cannot install guest Binder bootstrap");
        File environment = child(root, "init.environ.rc");
        String text = new String(java.nio.file.Files.readAllBytes(environment.toPath()), "UTF-8");
        String export = "    export LD_PRELOAD /system/lib64/libpocket_binder_bootstrap.so\n";
        if (!text.contains(export)) {
            if (text.contains("export LD_PRELOAD") || !text.contains("\non init\n"))
                throw new IOException("Unsupported guest init environment");
            File original = new File(root, "init.environ.rc.pocket-original");
            if (!original.exists()) java.nio.file.Files.copy(environment.toPath(), original.toPath());
            String patched = text.replace("\non init\n", "\non init\n" + export);
            File staged = new File(root, "init.environ.rc.new");
            try (OutputStream out = new FileOutputStream(staged)) { out.write(patched.getBytes("UTF-8")); }
            if (!staged.renameTo(environment)) throw new IOException("Cannot prepare guest init environment");
        }
        // Remove our earlier property-action experiment. This guest can reach
        // boot_completed=1 without delivering that init action. Query the
        // actual guest property via its own getprop instead.
        File init = child(root, "init.rc");
        String initText = new String(java.nio.file.Files.readAllBytes(init.toPath()), "UTF-8");
        if (initText.contains("\nimport /init.pocket.rc\n")) {
            java.nio.file.Files.write(init.toPath(),
                initText.replace("\nimport /init.pocket.rc\n", "\n").getBytes("UTF-8"));
        }
        new File(root, "init.pocket.rc").delete();
        File marker = child(root, "data/pocket-boot-complete");
        if (marker.exists() && !marker.delete()) throw new IOException("Cannot clear old boot marker");
        // Clear the prior property area BEFORE launching either the guest or
        // a status query, so an old boot_completed=1 cannot win a startup race.
        File properties = child(root, "dev/__properties__");
        if (properties.exists()) remove(properties);
        if (!properties.mkdirs()) throw new IOException("Cannot prepare guest property area");
    }
    private static File child(File root, String path) throws IOException {
        if (path.isEmpty() || path.startsWith("/") || path.contains("\\") ||
                Arrays.asList(path.split("/", -1)).contains("..") ||
                Arrays.asList(path.split("/", -1)).contains("."))
            throw new IOException("Invalid guest path");
        File file = new File(root, path);
        if (!file.getCanonicalPath().startsWith(root.getCanonicalPath() + "/"))
            throw new IOException("Guest path escapes image");
        return file;
    }
    static void install(File archive, File files) throws Exception {
        install(archive, files, "", null);
    }
    static void install(File archive, File files, Progress progress) throws Exception {
        install(archive, files, "", progress);
    }
    private static void install(File archive, File files, String prefix, Progress progress) throws Exception {
        File destination = new File(files, "rootfs");
        if (destination.exists()) throw new IOException("A guest is already installed");
        File work = new File(files, "import-" + UUID.randomUUID());
        if (!work.mkdir()) throw new IOException("Cannot create import directory");
        boolean success = false;
        try (ZipFile zip = new ZipFile(archive)) {
            ZipEntry layout = zip.getEntry(prefix + "pocket-layout.json");
            if (layout == null || layout.getSize() < 0 || layout.getSize() > 8 * 1024 * 1024)
                throw new IOException("Not an AOSP Pocket guest image");
            ByteArrayOutputStream buffer = new ByteArrayOutputStream();
            try (InputStream in = zip.getInputStream(layout)) { copy(in, buffer, 8 * 1024 * 1024); }
            JSONObject metadata = new JSONObject(buffer.toString("UTF-8"));
            if (metadata.getInt("format") != 1 || !metadata.getString("abi").equals("arm64-v8a"))
                throw new IOException("Unsupported guest image format/ABI");
            // Old format-1 API 25 images did not declare guest_sdk. Preserve
            // compatibility, checking the real build.prop after extraction.
            int declaredSdk = metadata.optInt("guest_sdk", -1);
            JSONObject entries = metadata.getJSONObject("entries");
            List<String> names = new ArrayList<>();
            for (Iterator<String> it = entries.keys(); it.hasNext();) names.add(it.next());
            Collections.sort(names);
            for (String name : names) {
                File file = child(work, name);
                if (entries.getJSONObject(name).getString("type").equals("directory") &&
                        !file.isDirectory() && !file.mkdirs()) throw new IOException("Cannot create " + name);
            }
            Set<String> seen = new HashSet<>();
            long total = 0, expected = 0;
            for (Enumeration<? extends ZipEntry> it = zip.entries(); it.hasMoreElements();) {
                ZipEntry entry = it.nextElement();
                if (entry.getName().startsWith(prefix) && !entry.getName().equals(prefix + "pocket-layout.json")) {
                    if (entry.getSize() < 0 || entry.getSize() > LIMIT - expected)
                        throw new IOException("Image exceeds size limit");
                    expected += entry.getSize();
                }
            }
            if (progress != null) progress.update(0, expected);
            for (Enumeration<? extends ZipEntry> it = zip.entries(); it.hasMoreElements();) {
                ZipEntry entry = it.nextElement();
                if (!entry.getName().startsWith(prefix) || entry.getName().equals(prefix + "pocket-layout.json")) continue;
                String name = entry.getName().substring(prefix.length());
                if (!prefix.isEmpty()) {
                    if (!name.endsWith(".payload")) throw new IOException("Unexpected bundled image entry");
                    name = name.substring(0, name.length() - ".payload".length());
                }
                JSONObject spec = entries.optJSONObject(name);
                if (spec == null || !spec.getString("type").equals("file") || !seen.add(name))
                    throw new IOException("Unexpected/duplicate image entry " + name);
                File file = child(work, name);
                File parent = file.getParentFile();
                if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create parent");
                try (InputStream in = zip.getInputStream(entry); OutputStream out = new FileOutputStream(file)) {
                    total += copy(in, out, LIMIT - total);
                }
                Os.chmod(file.getPath(), spec.getInt("mode") & 0777);
                if (progress != null) progress.update(total, expected);
            }
            for (String name : names) {
                JSONObject spec = entries.getJSONObject(name);
                if (spec.getString("type").equals("file") && !seen.contains(name))
                    throw new IOException("Missing image file " + name);
            }
            if (!new File(work, "init").exists() || !new File(work, "system/build.prop").isFile())
                throw new IOException("Guest lacks init or system/build.prop");
            GuestProfile profile = GuestProfile.detect(work);
            if (declaredSdk != -1) profile.checkDeclaredSdk(declaredSdk);
            profile.requireBootSupport();
            // Create links last: archive writes must never traverse guest links.
            for (String name : names) {
                JSONObject spec = entries.getJSONObject(name);
                if (spec.getString("type").equals("symlink")) {
                    File link = child(work, name);
                    Os.symlink(spec.getString("target"), link.getPath());
                }
            }
            // rootfs has to remain writable for future guest boot preparation.
            if (Thread.currentThread().isInterrupted()) throw new IOException("Image setup cancelled");
            if (!work.renameTo(destination)) throw new IOException("Cannot activate guest image");
            success = true;
        } finally { if (!success) remove(work); }
    }
    static long copy(InputStream in, OutputStream out, long limit) throws IOException {
        byte[] data = new byte[65536];
        long count = 0;
        for (int n; (n = in.read(data)) != -1;) {
            if (Thread.currentThread().isInterrupted()) throw new IOException("Image setup cancelled");
            count += n;
            if (count > limit) throw new IOException("Image exceeds size limit");
            out.write(data, 0, n);
        }
        return count;
    }
    private static void remove(File file) throws IOException {
        if (!java.nio.file.Files.isSymbolicLink(file.toPath()) && file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) for (File child : children) remove(child);
        }
        if (file.exists() || java.nio.file.Files.isSymbolicLink(file.toPath())) file.delete();
    }
}
