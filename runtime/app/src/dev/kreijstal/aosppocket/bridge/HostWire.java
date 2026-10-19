// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.bridge;

import java.io.*;

/** Versioned, bounded byte protocol shared by host and pinned guest adapters. */
public final class HostWire {
    private HostWire() { }
    public static final int MAGIC=0x504b4831, MAX=16*1024*1024;
    public static final int INFO=1, DNS=2, CONTACTS=3, LOCATION=4, PHONE=5,
        SENSORS=6, CAMERAS=7, CAMERA=8, MICROPHONE=9, PLAYBACK=10, VIBRATE=11, PHONE_UI=12;
    public static byte[] read(DataInputStream in,int limit) throws IOException {
        int length=in.readInt();
        if(length<0 || length>limit)throw new IOException("Host bridge packet exceeds limit");
        byte[] bytes=new byte[length];in.readFully(bytes);return bytes;
    }
    public static void write(DataOutputStream out,byte[] bytes) throws IOException {
        if(bytes.length>MAX)throw new IOException("Host bridge response exceeds limit");
        out.writeInt(bytes.length);out.write(bytes);
    }
}
