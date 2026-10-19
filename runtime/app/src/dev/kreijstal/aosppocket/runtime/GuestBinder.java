// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;

/** Compatibility with the Ananbox guest's Binder bootstrap protocol. */
final class GuestBinder extends BroadcastReceiver {
    static final String ACTION = "com.github.ananbox.BINDER";
    private IBinder manager;
    private final List<IBinder> pending = new ArrayList<>();

    synchronized void reset() { manager = null; pending.clear(); }

    @Override public synchronized void onReceive(Context context, Intent intent) {
        Bundle extras = intent.getExtras();
        if (extras == null) return;
        IBinder incoming = extras.getBinder("binder");
        if (incoming != null && incoming.pingBinder()) {
            // isBinderAlive() can retain a cached true result after the old
            // guest died. A newly published, pinged manager supersedes it.
            manager = incoming;
            Log.i("PocketRuntime", "Guest Binder service manager received");
            for (IBinder waiting : pending) handoff(waiting);
            pending.clear();
            return;
        }
        IBinder local = extras.getBinder("local");
        if (local == null) return;
        if (manager == null) pending.add(local);
        else handoff(local);
    }

    private void handoff(IBinder local) {
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeInterfaceToken("com.github.ananbox.ILocalInterface");
            request.writeStrongBinder(manager);
            if (!local.transact(IBinder.FIRST_CALL_TRANSACTION + 1, request, reply, 0))
                throw new IllegalStateException("Guest rejected Binder bootstrap");
            reply.readException();
        } catch (Exception error) { Log.e("PocketRuntime", "Guest Binder handoff failed", error); }
        finally { request.recycle(); reply.recycle(); }
    }

    static void prepare(Context context, File root) throws Exception {
        // Let the host's Stub construct its own proxy: its private constructor
        // is blocked by hidden-API enforcement on Android 16.
        final int[] code = {0};
        IBinder sink = (IBinder) Proxy.newProxyInstance(IBinder.class.getClassLoader(),
            new Class<?>[]{IBinder.class}, (object, method, args) -> {
                if ("transact".equals(method.getName())) { code[0] = (Integer) args[0]; return true; }
                if ("queryLocalInterface".equals(method.getName())) return null;
                throw new UnsupportedOperationException(method.getName());
            });
        Object capture = Class.forName("android.app.IActivityManager$Stub")
            .getMethod("asInterface", IBinder.class).invoke(null, sink);
        Class<?> proxyType = capture.getClass();
        Method broadcast = null;
        for (Method method : proxyType.getMethods())
            if (method.getName().equals("broadcastIntent")) broadcast = method;
        if (broadcast == null || broadcast.getParameterTypes().length != 13)
            throw new IllegalStateException("Unsupported host broadcastIntent ABI");
        broadcast.setAccessible(true);
        broadcast.invoke(capture, null, null, null, null, 0, null, null,
            null, 0, null, false, false, 0);
        if (code[0] <= 0) throw new IllegalStateException("Missing host broadcast transaction");
        for (String name : new String[]{"local", "binder"}) {
            Intent intent = new Intent(ACTION).setPackage(context.getPackageName());
            Bundle extras = new Bundle();
            extras.putBinder(name, new Binder());
            intent.putExtras(extras);
            Parcel p = Parcel.obtain();
            try {
                p.writeInterfaceToken("android.app.IActivityManager");
                p.writeStrongBinder(null);
                p.writeTypedObject(intent, 0);
                p.writeString(null);
                p.writeStrongBinder(null);
                p.writeInt(0);
                p.writeString(null);
                p.writeTypedObject(null, 0);
                p.writeStringArray(null);
                // IActivityManager.broadcastIntent appOp: OP_NONE is -1.
                // Zero is COARSE_LOCATION, which rejects this internal Binder
                // handoff when foreground-only location access becomes idle.
                p.writeInt(-1);
                p.writeTypedObject(new Bundle(), 0);
                p.writeInt(1);
                p.writeInt(0);
                p.writeInt(0);
                Native.dumpParcel(p, new File(root, name + "BroadcastIntent").getPath(), p.dataSize());
            } finally { p.recycle(); }
        }
        try (FileOutputStream out = new FileOutputStream(new File(root, "trans_code"))) {
            out.write(Integer.toString(code[0]).getBytes("UTF-8"));
            // The old guest reader uses sscanf on a fixed, uninitialized
            // buffer. Include its terminator in the file, not just EOF.
            out.write(0);
        }
    }
}
