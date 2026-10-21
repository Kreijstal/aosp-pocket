// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketattestationtest;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import java.io.*;
import dev.kreijstal.aosppocket.bridge.DisplayGeometry;

/** Calls the existing app-private guest installer as the authorized owner. */
public final class Control {
    public static void main(String[] args) {
        try(LocalSocket socket=new LocalSocket()) {
            socket.connect(new LocalSocketAddress("/dev/socket/pocket_control",LocalSocketAddress.Namespace.FILESYSTEM));
            socket.setSoTimeout(90000);
            DataInputStream in=new DataInputStream(socket.getInputStream());DataOutputStream out=new DataOutputStream(socket.getOutputStream());
            DisplayGeometry geometry=DisplayGeometry.read(in);
            System.out.println("Connected to guest "+geometry);
            if(args.length==2 && "--launch".equals(args[0])) {
                out.writeInt(9);out.writeUTF(args[1]);out.flush();
                if(!in.readBoolean())throw new IOException("Guest rejected launch");
                System.out.println("Launched "+args[1]);
            } else {
                out.writeInt(4);out.flush();System.out.println(in.readUTF());
            }
            System.exit(0);
        }catch(Throwable error){error.printStackTrace();System.exit(1);}
    }
}
