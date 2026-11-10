// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketvolume32;

import android.content.Context;
import org.json.JSONObject;

/** Ordinary in-process syscalls: no injected descriptors or privileged helper. */
final class NamespaceProof {
    static { System.loadLibrary("pocket_namespace_probe32"); }
    private static native String nativeRun(String external, String internal, String peer, String nonce);
    static JSONObject run(Context context, String nonce) throws Exception {
        return new JSONObject(nativeRun(context.getExternalFilesDir(null).getPath(),
            context.getFilesDir().getPath(),
            "/storage/emulated/0/Android/data/dev.kreijstal.pocketvolumeowner32/files/pocket-canary-"+nonce,
            nonce));
    }
}
