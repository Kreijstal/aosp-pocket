// SPDX-License-Identifier: GPL-3.0-only
// Compile-time stub of a hidden Android 9 interface; the guest framework's class is used at run time.
package android.hardware.display;
public interface IVirtualDisplayCallback extends android.os.IInterface {
    void onPaused() throws android.os.RemoteException;
    void onResumed() throws android.os.RemoteException;
    void onStopped() throws android.os.RemoteException;
    abstract class Stub extends android.os.Binder implements IVirtualDisplayCallback {
        public android.os.IBinder asBinder() { return this; }
    }
}
