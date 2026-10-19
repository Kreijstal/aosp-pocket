// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.bridge;

import java.io.*;
import java.nio.*;
import java.nio.channels.FileChannel;

/** Three coherent RGBA slots; the composer never overwrites a held front buffer. */
public class SharedFrames implements Closeable {
    public static final int SLOT_COUNT=3;
    public final int frameBytes;
    private final RandomAccessFile file;
    private final ByteBuffer mapping;
    protected SharedFrames(DisplayGeometry geometry,ByteBuffer memory) throws IOException {
        frameBytes=Math.multiplyExact(Math.multiplyExact(geometry.width,geometry.height),4);
        if(memory==null || memory.capacity()!=SLOT_COUNT*frameBytes)throw new IOException("Invalid shared display mapping");
        mapping=memory;file=null;
    }
    public SharedFrames(File path, DisplayGeometry geometry, boolean create) throws IOException {
        frameBytes=Math.multiplyExact(Math.multiplyExact(geometry.width,geometry.height),4);
        file=new RandomAccessFile(path,"rw");
        try {
            long length=(long)SLOT_COUNT*frameBytes;
            if(create)file.setLength(length);
            if(file.length()!=length)throw new IOException("Shared display buffer size does not match geometry");
            mapping=file.getChannel().map(FileChannel.MapMode.READ_WRITE,0,length);
        } catch(IOException | RuntimeException error) {file.close();throw error;}
    }
    public ByteBuffer slot(int index) {
        if(index<0 || index>=SLOT_COUNT)throw new IllegalArgumentException("Invalid display slot");
        ByteBuffer view=mapping.duplicate();view.position(index*frameBytes);view.limit((index+1)*frameBytes);
        return view.slice();
    }
    @Override public void close() throws IOException {if(file!=null)file.close();}
}
