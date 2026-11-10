// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.ParcelFileDescriptor;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Set;
import java.util.function.BooleanSupplier;

/** Finite owner operations over the existing authenticated guest control service. */
final class GuestManager32 {
    static boolean selected(File root)throws IOException {
        File marker=new File(root,"system/etc/pocket-manager-control32");
        if(!marker.exists())return false;
        if(marker.length()>64 || !new String(Files.readAllBytes(marker.toPath()),StandardCharsets.US_ASCII).equals("input-control-v1\n"))
            throw new IOException("Unsupported guest Manager protocol");
        return true;
    }
    static String execute(File root,int width,int height,Set<LocalSocket> sockets,BooleanSupplier closed,File log,String[] args)throws IOException {
        if(args.length<1 || args.length>2)throw new IOException("Invalid guest Manager arguments");
        byte[][] values=new byte[args.length][];
        for(int n=0;n<args.length;n++) {
            values[n]=args[n].getBytes(StandardCharsets.UTF_8);
            if(values[n].length<1 || values[n].length>1024)throw new IOException("Guest Manager argument exceeds limit");
        }
        LocalSocket peer=new LocalSocket();sockets.add(peer);
        try(LocalSocket socket=peer) {
            if(closed.getAsBoolean())throw new IOException("Guest stopped");
            socket.connect(new LocalSocketAddress(new File(root,"dev/socket/pocket_control32").getPath(),LocalSocketAddress.Namespace.FILESYSTEM));
            // Startup readiness and PackageInstaller/broadcast delivery each
            // have a 120s deadline. Allow both plus transport time on this
            // worker channel; input uses a separate connection and deadline.
            socket.setSoTimeout(300000);
            DataInputStream in=new DataInputStream(socket.getInputStream());DataOutputStream out=new DataOutputStream(socket.getOutputStream());
            if(in.readInt()!=0x504b4932 || in.readInt()!=width || in.readInt()!=height)throw new IOException("Guest Manager geometry mismatch");
            try(ParcelFileDescriptor lifetime=ParcelFileDescriptor.open(new File("/proc/self/stat"),ParcelFileDescriptor.MODE_READ_ONLY)) {
                socket.setFileDescriptorsForSend(new FileDescriptor[]{lifetime.getFileDescriptor()});
                socket.getOutputStream().write(0);socket.setFileDescriptorsForSend(null);
                if(!in.readBoolean())throw new IOException("Guest Manager lifetime rejected");
            }
            out.writeInt(4);out.writeInt(values.length);
            for(byte[] value:values){out.writeInt(value.length);out.write(value);}out.flush();
            int status=in.readInt(),size=in.readInt();
            if((status!=0 && status!=1) || size<0 || size>65536)throw new IOException("Invalid guest Manager response");
            byte[] bytes=new byte[size];in.readFully(bytes);Files.write(log.toPath(),bytes);
            String text=new String(bytes,StandardCharsets.UTF_8);
            // The operation may already be committed. Never retry through the
            // old executable after a transport or command failure.
            if(status!=0)throw new IOException(text.length()>1000?text.substring(text.length()-1000):text);
            for(String line:text.split("\n"))if(line.startsWith("POCKET_MANAGER32_RESULT "))return line.substring("POCKET_MANAGER32_RESULT ".length());
            throw new IOException("Guest Manager returned no result");
        }finally{sockets.remove(peer);}
    }
}
