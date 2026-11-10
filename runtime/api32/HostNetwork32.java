// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.content.Context;
import android.content.ContentProviderClient;
import android.content.ContentResolver;
import android.net.*;
import android.os.*;
import dev.kreijstal.aosppocket.bridge.HostWire;
import java.io.*;
import java.lang.reflect.*;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import org.json.*;

/** Publishes the shared host uplink using the actual 12L NetworkAgent protocol. */
final class HostNetwork32 {
    private static final String INTERFACE="pocket0";
    private final Context context;
    private final HandlerThread thread=new HandlerThread("PocketHostNetwork32");
    private final Handler handler;
    private Agent agent;
    private Network network;
    private String previous,error;
    HostNetwork32(Context context)throws Exception {
        if(Build.VERSION.SDK_INT!=32)throw new IOException("Expected Android 12L");
        this.context=context;configureMonitor(context);thread.start();handler=new Handler(thread.getLooper());
        handler.post(new Runnable(){public void run(){refresh();handler.postDelayed(this,2000);}});
    }
    private static void configureMonitor(Context context)throws Exception {
        // Kernel fwmarks cannot identify this shared host transport. Stock
        // 12L supports DNS-only stall detection; Internet validation stays on.
        Class<?> api=Class.forName("android.app.IActivityManager");
        Object manager=Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
        IBinder token=new Binder();boolean acquired=false;
        try {
            Object holder=api.getMethod("getContentProviderExternal",String.class,int.class,IBinder.class,String.class)
                .invoke(manager,"settings",0,token,"pocket-network-profile");
            if(holder==null)throw new IOException("Settings provider is unavailable for network profile");
            acquired=true;Object provider=holder.getClass().getField("provider").get(holder);
            try(ContentProviderClient client=(ContentProviderClient)ContentProviderClient.class.getConstructor(ContentResolver.class,
                    Class.forName("android.content.IContentProvider"),String.class,boolean.class)
                    .newInstance(context.getContentResolver(),provider,"settings",true)) {
                String key="connectivity/data_stall_evaluation_type";
                Bundle args=new Bundle();args.putInt("_user",0);args.putString("value","1");
                client.call("PUT_config",key,args);Bundle value=client.call("GET_config",key,args);
                if(value==null || !"1".equals(value.getString("value")))throw new IOException("DNS-only stall monitor profile did not persist");
            }
        }finally {
            if(acquired)api.getMethod("removeContentProviderExternalAsUser",String.class,IBinder.class,int.class)
                .invoke(manager,"settings",token,0);
        }
        System.out.println("API32_HOST_NETWORK_PROFILE dns_stall_detection=enabled kernel_tcp_stall_detection=unsupported internet_validation=enabled");
    }
    private static JSONObject uplink()throws Exception {
        try(LocalSocket socket=new LocalSocket()) {
            socket.connect(new LocalSocketAddress("/dev/socket/pocket_host",LocalSocketAddress.Namespace.FILESYSTEM));
            socket.setSoTimeout(5000);
            DataOutputStream out=new DataOutputStream(socket.getOutputStream());
            DataInputStream in=new DataInputStream(socket.getInputStream());
            out.writeInt(HostWire.MAGIC);out.writeInt(HostWire.INFO);HostWire.write(out,"{}".getBytes(StandardCharsets.UTF_8));out.flush();
            int status=in.readInt();byte[] bytes=HostWire.read(in,65536);
            if(status!=0)throw new IOException("Host uplink query failed: "+status);
            return new JSONObject(new String(bytes,StandardCharsets.UTF_8)).getJSONObject("network");
        }
    }
    private static NetworkCapabilities capabilities(JSONObject host)throws Exception {
        NetworkCapabilities caps=new NetworkCapabilities();
        Method addTransport=NetworkCapabilities.class.getMethod("addTransportType",int.class);
        Method add=NetworkCapabilities.class.getMethod("addCapability",int.class);
        addTransport.invoke(caps,NetworkCapabilities.TRANSPORT_ETHERNET);
        add.invoke(caps,NetworkCapabilities.NET_CAPABILITY_NOT_RESTRICTED);
        add.invoke(caps,NetworkCapabilities.NET_CAPABILITY_TRUSTED);
        add.invoke(caps,NetworkCapabilities.NET_CAPABILITY_NOT_VPN);
        add.invoke(caps,NetworkCapabilities.class.getField("NET_CAPABILITY_NOT_VCN_MANAGED").getInt(null));
        if(!host.getBoolean("suspended"))add.invoke(caps,NetworkCapabilities.NET_CAPABILITY_NOT_SUSPENDED);
        if(!host.getBoolean("roaming"))add.invoke(caps,NetworkCapabilities.NET_CAPABILITY_NOT_ROAMING);
        if(!host.getBoolean("congested"))add.invoke(caps,NetworkCapabilities.NET_CAPABILITY_NOT_CONGESTED);
        if(host.getBoolean("internet"))add.invoke(caps,NetworkCapabilities.NET_CAPABILITY_INTERNET);
        if(!host.getBoolean("metered"))add.invoke(caps,NetworkCapabilities.NET_CAPABILITY_NOT_METERED);
        // VALIDATED belongs to the guest's NetworkMonitor; never manufacture it.
        return caps;
    }
    private static LinkProperties properties(JSONObject host)throws Exception {
        LinkProperties link=new LinkProperties();
        LinkProperties.class.getMethod("setInterfaceName",String.class).invoke(link,INTERFACE);
        Method addAddress=LinkProperties.class.getMethod("addLinkAddress",LinkAddress.class);
        Constructor<?> address=LinkAddress.class.getConstructor(String.class);
        JSONArray addresses=host.getJSONArray("addresses");
        for(int i=0;i<addresses.length();i++)addAddress.invoke(link,address.newInstance(addresses.getString(i)));
        Method addDns=LinkProperties.class.getMethod("addDnsServer",InetAddress.class);
        JSONArray dns=host.getJSONArray("dns");
        for(int i=0;i<dns.length();i++)addDns.invoke(link,InetAddress.getByName(dns.getString(i)));
        Constructor<?> prefix=IpPrefix.class.getConstructor(String.class);
        Constructor<?> route=RouteInfo.class.getConstructor(IpPrefix.class,InetAddress.class,String.class,int.class);
        Method addRoute=LinkProperties.class.getMethod("addRoute",RouteInfo.class);
        JSONArray routes=host.getJSONArray("routes");
        for(int i=0;i<routes.length();i++) {
            JSONObject row=routes.getJSONObject(i);String gateway=row.optString("gateway");
            addRoute.invoke(link,route.newInstance(prefix.newInstance(row.getString("destination")),
                gateway.isEmpty()?null:InetAddress.getByName(gateway),INTERFACE,row.getInt("type")));
        }
        int mtu=host.optInt("mtu");
        if(mtu>0 && mtu<=65535)LinkProperties.class.getMethod("setMtu",int.class).invoke(link,mtu);
        return link;
    }
    private final class Agent extends NetworkAgent {
        Agent(NetworkCapabilities caps,LinkProperties link,NetworkAgentConfig config) {
            super(context,thread.getLooper(),"PocketHostUplink32",caps,link,60,config,null);
        }
        private void retired() {
            if(agent!=this)return;
            agent=null;network=null;previous=null;
            System.out.println("API32_HOST_NETWORK_RETIRED transport=shared_host");
        }
        @Override public void onNetworkUnwanted(){retired();}
        @Override public void onNetworkDestroyed(){retired();}
    }
    private void disconnect()throws Exception {
        Agent old=agent;agent=null;network=null;previous=null;
        if(old!=null) {
            old.unregister();
            System.out.println("API32_HOST_NETWORK_DISCONNECTED transport=shared_host");
        }
    }
    private void refresh() {
        try {
            JSONObject host=uplink();
            if(!host.getBoolean("connected")){disconnect();error=null;return;}
            String current=host.toString();if(agent!=null && current.equals(previous))return;
            NetworkCapabilities caps=capabilities(host);LinkProperties link=properties(host);
            if(agent==null) {
                Class<?> builderClass=Class.forName("android.net.NetworkAgentConfig$Builder");Object builder=builderClass.getConstructor().newInstance();
                builderClass.getMethod("setLegacyType",int.class).invoke(builder,ConnectivityManager.TYPE_ETHERNET);
                builderClass.getMethod("setLegacyTypeName",String.class).invoke(builder,"HOST");
                builderClass.getMethod("setLegacySubTypeName",String.class).invoke(builder,"Shared host uplink");
                builderClass.getMethod("setProvisioningNotificationEnabled",boolean.class).invoke(builder,false);
                builderClass.getMethod("setNat64DetectionEnabled",boolean.class).invoke(builder,false);
                NetworkAgentConfig config=(NetworkAgentConfig)builderClass.getMethod("build").invoke(builder);
                agent=new Agent(caps,link,config);network=agent.register();
                if(network==null)throw new IOException("NetworkAgent registration returned no network");
                agent.markConnected();
                System.out.println("API32_HOST_NETWORK_REGISTERED netid="+network+" transport=shared_host");
            }else {
                agent.sendLinkProperties(link);agent.sendNetworkCapabilities(caps);
            }
            previous=current;error=null;
        }catch(Exception failure) {
            try{disconnect();}catch(Exception cleanup){failure.addSuppressed(cleanup);}
            Throwable reason=failure instanceof InvocationTargetException?failure.getCause():failure;
            String message=String.valueOf(reason);
            if(!message.equals(error))System.err.println("API32_HOST_NETWORK_UNAVAILABLE "+message);
            error=message;
        }
    }
}
