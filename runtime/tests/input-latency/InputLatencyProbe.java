// SPDX-License-Identifier: GPL-3.0-only
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.SystemClock;
import java.io.*;
import java.nio.file.Files;
import org.json.*;
import dev.kreijstal.aosppocket.bridge.DisplayGeometry;

/** Measures the real production control socket against a slow ordinary app. */
public final class InputLatencyProbe {
    public static void main(String[] args) {
        try {
            if(args.length>0 && args[0].equals("--back")) {
                try(LocalSocket socket=new LocalSocket()) {
                    socket.connect(new LocalSocketAddress("/dev/socket/pocket_control",LocalSocketAddress.Namespace.FILESYSTEM));
                    socket.setSoTimeout(15000);
                    DataInputStream in=new DataInputStream(socket.getInputStream());
                    DataOutputStream out=new DataOutputStream(socket.getOutputStream());
                    DisplayGeometry.read(in);out.writeInt(3);out.writeInt(4);out.flush();
                    if(!in.readBoolean())throw new IOException("Guest navigation submission rejected");
                }
                System.out.println("INPUT_NAVIGATION_ACCEPTED");System.exit(0);
            }
            File directory=new File("/data/user/0/dev.kreijstal.pocketinputlatency/files");
            File result=new File(directory,"result.json");result.delete();
            if(!new File(directory,"ready.json").isFile())throw new IOException("Latency test activity is not ready");
            JSONArray timings=new JSONArray();
            try(LocalSocket socket=new LocalSocket()) {
                socket.connect(new LocalSocketAddress("/dev/socket/pocket_control",LocalSocketAddress.Namespace.FILESYSTEM));
                socket.setSoTimeout(15000);
                DataInputStream in=new DataInputStream(socket.getInputStream());
                DataOutputStream out=new DataOutputStream(socket.getOutputStream());
                DisplayGeometry size=DisplayGeometry.read(in);long down=SystemClock.uptimeMillis();
                for(int action:new int[]{0,1}) {
                    long began=SystemClock.elapsedRealtimeNanos();
                    out.writeInt(2);out.writeLong(down);out.writeLong(SystemClock.uptimeMillis());
                    out.writeInt(action);out.writeInt(1);out.writeInt(0);
                    out.writeFloat(size.width/2f);out.writeFloat(size.height/2f);out.flush();
                    if(!in.readBoolean())throw new IOException("Guest input submission rejected");
                    timings.put((SystemClock.elapsedRealtimeNanos()-began)/1000000.0);
                }
            }
            long deadline=SystemClock.uptimeMillis()+20000;
            while(!result.isFile() && SystemClock.uptimeMillis()<deadline)Thread.sleep(50);
            JSONObject delivered=new JSONObject(new String(Files.readAllBytes(result.toPath()),"UTF-8"));
            if(!delivered.getBoolean("orderedDownUp"))throw new AssertionError("App did not receive ordered down/up events");
            boolean asynchronous=args.length>0 && args[0].equals("--expect-async");
            double first=timings.getDouble(0);
            if(asynchronous && first>=1000)throw new AssertionError("Input still waits behind the slow callback: "+first);
            if(!asynchronous && first<1000)throw new AssertionError("Baseline did not wait for the slow callback: "+first);
            System.out.println("INPUT_LATENCY_RESULT "+new JSONObject().put("validation","PASS")
                    .put("mode",asynchronous?"async":"baseline").put("acknowledgementMs",timings).put("app",delivered));
            System.exit(0);
        } catch(Throwable failure){failure.printStackTrace();System.exit(1);}
    }
}
