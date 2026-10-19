// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.bridge;

import android.os.ParcelFileDescriptor;
import java.io.*;
import java.nio.ByteBuffer;

/** Anonymous shared memory passed over the app-private socket with SCM_RIGHTS. */
public final class AnonymousFrames extends SharedFrames {
    static {System.loadLibrary("pocket_frames");}
    private final ByteBuffer memory;
    private final ParcelFileDescriptor descriptor;
    private boolean closed;
    private AnonymousFrames(DisplayGeometry geometry,ByteBuffer memory,ParcelFileDescriptor descriptor) throws IOException {
        super(geometry,memory);this.memory=memory;this.descriptor=descriptor;
    }
    public static AnonymousFrames create(DisplayGeometry geometry) throws IOException {
        int bytes=Math.multiplyExact(Math.multiplyExact(geometry.width,geometry.height),8);
        ParcelFileDescriptor descriptor=ParcelFileDescriptor.adoptFd(createRegion(bytes));
        try {return new AnonymousFrames(geometry,mapRegion(descriptor.getFileDescriptor(),bytes),descriptor);}
        catch(IOException | RuntimeException error){descriptor.close();throw error;}
    }
    public static AnonymousFrames attach(DisplayGeometry geometry,FileDescriptor descriptor) throws IOException {
        int bytes=Math.multiplyExact(Math.multiplyExact(geometry.width,geometry.height),8);
        return new AnonymousFrames(geometry,mapRegion(descriptor,bytes),null);
    }
    public FileDescriptor descriptor(){return descriptor.getFileDescriptor();}
    @Override public void close() throws IOException {
        if(closed)return;closed=true;unmapRegion(memory);
        if(descriptor!=null)descriptor.close();
    }
    private static native int createRegion(int bytes) throws IOException;
    private static native ByteBuffer mapRegion(FileDescriptor descriptor,int bytes) throws IOException;
    private static native void unmapRegion(ByteBuffer memory);
}
