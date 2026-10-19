// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.hostphone;

import android.app.Activity;
import android.content.Intent;
import android.net.*;
import android.os.Bundle;
import android.widget.TextView;
import dev.kreijstal.aosppocket.bridge.HostWire;
import org.json.JSONObject;
import java.io.*;
import java.nio.charset.StandardCharsets;

/** Calls and SMS stay on the real host SIM/IMS stack, with host confirmation. */
public final class MainActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);TextView status=new TextView(this);
        status.setText("Opening the host phone app. Confirm the call or message there.");setContentView(status);
        Intent intent=getIntent();
        new Thread(()->{
            try {
                Uri data=intent.getData();String scheme=data==null?"tel":data.getScheme();
                boolean sms="sms".equals(scheme) || "smsto".equals(scheme);
                JSONObject args=new JSONObject().put("action",sms?"sms":"dial")
                    .put("address",data==null?"":data.getSchemeSpecificPart())
                    .put("body",intent.getStringExtra("sms_body"));
                try(LocalSocket socket=new LocalSocket()) {
                    socket.connect(new LocalSocketAddress("/dev/socket/pocket_host",LocalSocketAddress.Namespace.FILESYSTEM));
                    socket.setSoTimeout(10000);
                    DataOutputStream out=new DataOutputStream(socket.getOutputStream());
                    DataInputStream in=new DataInputStream(socket.getInputStream());
                    out.writeInt(HostWire.MAGIC);out.writeInt(HostWire.PHONE_UI);
                    HostWire.write(out,args.toString().getBytes(StandardCharsets.UTF_8));out.flush();
                    int code=in.readInt();byte[] response=HostWire.read(in,8192);
                    if(code!=0)throw new IOException(new String(response,StandardCharsets.UTF_8));
                }
                runOnUiThread(this::finish);
            } catch(Exception error){runOnUiThread(()->status.setText("Host phone unavailable: "+error.getMessage()));}
        },"PocketHostPhone").start();
    }
}
