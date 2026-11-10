// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import java.io.File;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.nio.charset.StandardCharsets;

/** Collects liblog datagrams inside this app's private fixture filesystem. */
public class GuestLog32 implements AutoCloseable {
    private final File path;
    private final LocalSocket socket=new LocalSocket(LocalSocket.SOCKET_DGRAM);
    private final StringBuilder output=new StringBuilder();
    private final Thread worker;
    private volatile boolean closed;

    public GuestLog32(File root)throws Exception {
        path=new File(root,"dev/socket/logdw");
        if(java.nio.file.Files.isSymbolicLink(path.toPath()))throw new java.io.IOException("Unexpected log socket symlink");
        if(path.exists() && !path.delete())throw new java.io.IOException("Cannot remove old fixture log socket");
        socket.bind(new LocalSocketAddress(path.getPath(),LocalSocketAddress.Namespace.FILESYSTEM));
        socket.setSoTimeout(200);
        InputStream input=socket.getInputStream();
        worker=new Thread(()->{
            byte[] packet=new byte[65536];
            while(!closed){
                try{
                    int length=input.read(packet);
                    if(length<14)continue;
                    int id=packet[0]&255;
                    if(id==2 || id==5 || id==6)continue;
                    int end=length;
                    while(end>12 && packet[end-1]==0)end--;
                    String message=new String(packet,12,end-12,StandardCharsets.UTF_8).replace('\0',':');
                    synchronized(output){if(output.length()<2*1024*1024)output.append(message).append('\n');}
                }catch(InterruptedIOException timeout){/* Poll close state. */}
                catch(Exception error){
                    // LocalSocket's native read reports SO_RCVTIMEO as a plain
                    // IOException(EAGAIN) on the Android 16 validation host.
                    if(!closed && error instanceof java.io.IOException && "Try again".equals(error.getMessage()))continue;
                    if(!closed)synchronized(output){output.append("LOG_COLLECTOR_ERROR ").append(error).append('\n');}
                    return;
                }
            }
        },"PocketApi32NativeLog");
        worker.setDaemon(true);worker.start();
    }
    public String text(){synchronized(output){return output.toString();}}
    @Override public void close()throws Exception {
        closed=true;socket.close();worker.join(2000);
        if(!path.delete() && path.exists())throw new java.io.IOException("Cannot remove fixture log socket");
    }
}
