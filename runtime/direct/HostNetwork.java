// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.content.Context;
import android.net.*;
import android.os.*;
import dev.kreijstal.aosppocket.bridge.HostWire;
import org.json.*;
import java.lang.reflect.*;

/** Android 9 NetworkAgent protocol, representing the host app's shared uplink. */
final class HostNetwork {
    private final Context context;
    private final HandlerThread thread=new HandlerThread("PocketHostNetwork");
    private final Handler handler;
    private Object channel;
    private NetworkInfo info;
    private LinkProperties link;
    private NetworkCapabilities caps;
    private String previous;
    private boolean registered;
    HostNetwork(Context owner) {
        context=owner;thread.start();
        handler=new Handler(thread.getLooper()) {
            @Override public void handleMessage(Message message) {
                try {
                    Class<?> ac=Class.forName("com.android.internal.util.AsyncChannel");
                    if(message.what==ac.getField("CMD_CHANNEL_FULL_CONNECTION").getInt(null)) {
                        channel=ac.getConstructor().newInstance();
                        ac.getMethod("connected",Context.class,Handler.class,Messenger.class).invoke(channel,context,this,message.replyTo);
                        ac.getMethod("replyToMessage",Message.class,int.class,int.class).invoke(channel,message,
                            ac.getField("CMD_CHANNEL_FULLY_CONNECTED").getInt(null),0);
                        publish();
                    } else if(message.what==ac.getField("CMD_CHANNEL_DISCONNECT").getInt(null)) {
                        if(channel!=null)ac.getMethod("disconnect").invoke(channel);channel=null;registered=false;
                    }
                } catch(Exception error){System.err.println("POCKET_HOST_NETWORK_CHANNEL_UNAVAILABLE "+error.getClass().getSimpleName());}
            }
        };
        handler.post(new Runnable(){public void run(){refresh();handler.postDelayed(this,2000);}});
    }
    private static NetworkCapabilities capabilities(JSONObject uplink) throws Exception {
        NetworkCapabilities caps=new NetworkCapabilities();
        NetworkCapabilities.class.getMethod("addTransportType",int.class).invoke(caps,NetworkCapabilities.TRANSPORT_ETHERNET);
        Method add=NetworkCapabilities.class.getMethod("addCapability",int.class);
        add.invoke(caps,NetworkCapabilities.NET_CAPABILITY_NOT_RESTRICTED);
        add.invoke(caps,NetworkCapabilities.NET_CAPABILITY_TRUSTED);
        if(uplink.optBoolean("internet"))add.invoke(caps,NetworkCapabilities.NET_CAPABILITY_INTERNET);
        if(!uplink.optBoolean("metered",true))add.invoke(caps,NetworkCapabilities.NET_CAPABILITY_NOT_METERED);
        return caps;
    }
    private void refresh() {
        try {
            JSONObject uplink=HostDevices.query(HostWire.INFO,new JSONObject()).getJSONObject("network");
            String snapshot=uplink.toString();
            if(snapshot.equals(previous) && registered)return;
            java.nio.file.Files.write(java.nio.file.Paths.get("/data/pocket-host-network.json"),snapshot.getBytes("UTF-8"));
            boolean connected=uplink.getBoolean("connected");
            info=new NetworkInfo(ConnectivityManager.TYPE_ETHERNET,0,"HOST","shared host app network");
            NetworkInfo.class.getMethod("setIsAvailable",boolean.class).invoke(info,connected);
            info.setDetailedState(connected?NetworkInfo.DetailedState.CONNECTED:NetworkInfo.DetailedState.DISCONNECTED,null,null);
            link=new LinkProperties();link.setInterfaceName("pocket0");
            JSONArray dns=uplink.getJSONArray("dns");
            for(int i=0;i<dns.length();i++)LinkProperties.class.getMethod("addDnsServer",java.net.InetAddress.class)
                .invoke(link,java.net.InetAddress.getByName(dns.getString(i)));
            caps=capabilities(uplink);
            if(!registered && connected) {
                Class<?> miscClass=Class.forName("android.net.NetworkMisc");Object misc=miscClass.getConstructor().newInstance();
                miscClass.getField("provisioningNotificationDisabled").setBoolean(misc,true);
                ConnectivityManager manager=(ConnectivityManager)context.getSystemService(Context.CONNECTIVITY_SERVICE);
                int id=(Integer)ConnectivityManager.class.getMethod("registerNetworkAgent",Messenger.class,NetworkInfo.class,
                    LinkProperties.class,NetworkCapabilities.class,int.class,miscClass)
                    .invoke(manager,new Messenger(handler),info,link,caps,60,misc);
                registered=true;System.out.println("POCKET_HOST_NETWORK_REGISTERED netid="+id+" transport=shared_host");
            }
            publish();
            previous=snapshot;
        } catch(Exception error){System.err.println("POCKET_HOST_NETWORK_UNAVAILABLE "+error.getClass().getSimpleName()+" "+error.getMessage());}
    }
    private void publish() throws Exception {
        if(channel==null || info==null)return;
        Class<?> agent=Class.forName("android.net.NetworkAgent");
        Method send=channel.getClass().getMethod("sendMessage",int.class,Object.class);
        send.invoke(channel,agent.getField("EVENT_NETWORK_CAPABILITIES_CHANGED").getInt(null),caps);
        send.invoke(channel,agent.getField("EVENT_NETWORK_PROPERTIES_CHANGED").getInt(null),link);
        send.invoke(channel,agent.getField("EVENT_NETWORK_INFO_CHANGED").getInt(null),info);
    }
}
