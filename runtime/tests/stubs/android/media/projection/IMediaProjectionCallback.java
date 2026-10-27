// SPDX-License-Identifier: GPL-3.0-only
// Compile-time stub of a hidden Android 9 interface; the guest framework's class is used at run time.
package android.media.projection;
public interface IMediaProjectionCallback extends android.os.IInterface {
    void onStop() throws android.os.RemoteException;
    abstract class Stub extends android.os.Binder implements IMediaProjectionCallback {
        public android.os.IBinder asBinder() { return this; }
    }
}
