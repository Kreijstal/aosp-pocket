// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import android.os.Parcel;
import android.view.Surface;
final class Native {
    static { System.loadLibrary("pocket_runtime"); }
    static native void configureHost();
    static native void initialize(String directory, int width, int height, int dpi);
    static native void attach(long displayId, Surface surface);
    static native void detach(long displayId);
    static native long createFramePresenter();
    static native void destroyFramePresenter(long presenter);
    static native String[] framePresenterInfo(long presenter);
    static native boolean presentFrame(long presenter, Surface surface, java.nio.ByteBuffer pixels, int width, int height);
    static native void touch(int action, int id, int x, int y);
    static native void dumpParcel(Parcel parcel, String destination, int size);
}
