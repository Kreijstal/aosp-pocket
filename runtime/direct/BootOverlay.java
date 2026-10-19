// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.content.*;
import android.system.Os;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

/** Explicit, trusted local-development image updates for this isolated probe. */
final class BootOverlay {
    private static File target(File root,String name) throws IOException {
        if (!(name.startsWith("system/") || name.startsWith("vendor/")) || name.contains("\\") || name.contains("\0"))
            throw new IOException("Invalid overlay path: "+name);
        for (String part:name.split("/")) if (part.equals("..") || part.equals(".") || part.isEmpty())
            throw new IOException("Invalid overlay component");
        File file=new File(root,name);
        if (!file.getCanonicalPath().startsWith(root.getCanonicalPath()+"/")) throw new IOException("Overlay escapes guest root");
        if (file.isDirectory()) throw new IOException("Overlay target is a directory");
        for (File parent=file;parent!=null && !parent.equals(root);parent=parent.getParentFile())
            if (Files.isSymbolicLink(parent.toPath())) throw new IOException("Overlay target contains symlink");
        return file;
    }
    static void reset(Context context,File root) throws Exception {
        SharedPreferences state=context.getSharedPreferences("direct-overlay",Context.MODE_PRIVATE);
        SharedPreferences.Editor cache=context.getSharedPreferences("api28-fixture-cache",Context.MODE_PRIVATE).edit();
        for (String name:state.getStringSet("files",Collections.emptySet())) {
            Files.deleteIfExists(target(root,name).toPath());cache.remove(name);
        }
        cache.commit();state.edit().clear().commit();
    }
    static List<String> apply(Context context,File root) throws Exception {
        File external=context.getExternalFilesDir(null);
        if (external==null) throw new IOException("External overlay staging unavailable");
        List<String> names=new ArrayList<>();
        try (ZipFile zip=new ZipFile(new File(external,"native-overlay.zip"))) {
            Set<String> unique=new HashSet<>();
            for (Enumeration<? extends ZipEntry> all=zip.entries();all.hasMoreElements();) {
                ZipEntry entry=all.nextElement();String name=entry.getName();
                if (name.equals("manifest.json")) continue;
                if (entry.isDirectory() || !unique.add(name)) throw new IOException("Invalid/duplicate overlay entry");
                target(root,name);names.add(name);
            }
            // Save the complete plan first: an interrupted copy is restored from
            // the APK on the next run, including same-size native replacements.
            context.getSharedPreferences("direct-overlay",Context.MODE_PRIVATE).edit().putStringSet("files",unique).commit();
            for (String name:names) {
                File file=target(root,name);file.getParentFile().mkdirs();
                File temporary=new File(file.getPath()+".pocket-overlay-tmp");
                try (InputStream input=zip.getInputStream(zip.getEntry(name))) {
                    Files.copy(input,temporary.toPath(),StandardCopyOption.REPLACE_EXISTING);
                }
                Os.chmod(temporary.getPath(),0755);
                Files.move(temporary.toPath(),file.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
            }
        }
        return names;
    }
}
