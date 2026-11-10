// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.content.Context;
import android.net.*;
import dev.kreijstal.aosppocket.bridge.HostWire;
import java.io.*;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.util.Set;
import java.util.concurrent.*;
import org.json.*;

/** Real host resolver and network snapshot, shared with the full host broker. */
public final class HostDns implements AutoCloseable {
    private final Context context;
    private final File endpoint;
    private final File root;
    private final LocalSocket bound=new LocalSocket();
    private final LocalServerSocket server;
    private final Set<LocalSocket> clients=ConcurrentHashMap.newKeySet();
    private final ThreadPoolExecutor workers=new ThreadPoolExecutor(4,4,0,TimeUnit.SECONDS,new ArrayBlockingQueue<>(8));
    private volatile boolean closed;

    /** DNS-only endpoint for isolated profiles without the full HostBroker. */
    public HostDns(Context owner,File root)throws IOException {
        context=owner.getApplicationContext();this.root=root;endpoint=new File(root,"dev/socket/pocket_host");
        if(endpoint.exists() && !endpoint.delete())throw new IOException("Cannot reset host DNS endpoint");
        bound.bind(new LocalSocketAddress(endpoint.getPath(),LocalSocketAddress.Namespace.FILESYSTEM));
        server=new LocalServerSocket(bound.getFileDescriptor());
        new Thread(this::accept,"PocketHostDns").start();
    }
    public static byte[] resolve(byte[] request)throws IOException {
        String hostname=new String(request,StandardCharsets.UTF_8);
        if(hostname.isEmpty() || hostname.length()>253 || !hostname.matches("[A-Za-z0-9_.:%-]+"))
            throw new IllegalArgumentException("Invalid hostname");
        InetAddress[] addresses=InetAddress.getAllByName(hostname);
        return addresses(addresses);
    }
    private static byte[] addresses(InetAddress[] addresses)throws IOException {
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();DataOutputStream data=new DataOutputStream(bytes);
        data.writeInt(Math.min(addresses.length,64));
        for(int i=0;i<Math.min(addresses.length,64);i++)HostWire.write(data,addresses[i].getAddress());
        return bytes.toByteArray();
    }
    public static byte[] servers(Context context)throws IOException {
        ConnectivityManager manager=(ConnectivityManager)context.getSystemService(Context.CONNECTIVITY_SERVICE);
        Network active=manager.getActiveNetwork();LinkProperties link=active==null?null:manager.getLinkProperties(active);
        return addresses(link==null?new InetAddress[0]:link.getDnsServers().toArray(new InetAddress[0]));
    }
    static boolean api32(File root)throws IOException {
        File properties=new File(root,"system/build.prop");
        if(!properties.isFile())return false;
        for(String line:Files.readAllLines(properties.toPath(),StandardCharsets.UTF_8))
            if(line.trim().equals("ro.build.version.sdk=32"))return true;
        return false;
    }
    static boolean trustedPeer(File root,int pid) {
        return trustedPeer(root,pid,0,1000);
    }
    static boolean trustedPeer(File root,int pid,int... permitted) {
        try {
            if(pid<=0)return false;
            byte[] bytes=Files.readAllBytes(new File(root,".pocket-identity/"+pid).toPath());
            if(bytes.length!=24)return false;
            ByteBuffer record=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
            if(record.getInt()!=0x504b4944 || record.getInt()!=pid)return false;
            int uid=record.getInt();record.getInt();long start=record.getLong();
            String stat=new String(Files.readAllBytes(new File("/proc/"+pid+"/stat").toPath()),StandardCharsets.UTF_8);
            int end=stat.lastIndexOf(')');if(end<0)return false;
            String[] fields=stat.substring(end+1).trim().split("\\s+");
            boolean allowed=false;for(int role:permitted)if(uid==role)allowed=true;
            return allowed && fields.length>=20 && Long.parseLong(fields[19])==start;
        }catch(Exception unavailable){return false;}
    }
    public static JSONObject network(Context context)throws JSONException {
        ConnectivityManager manager=(ConnectivityManager)context.getSystemService(Context.CONNECTIVITY_SERVICE);
        Network active=manager.getActiveNetwork();NetworkCapabilities caps=active==null?null:manager.getNetworkCapabilities(active);
        JSONObject result=new JSONObject().put("connected",active!=null && caps!=null)
            .put("metered",manager.isActiveNetworkMetered())
            .put("internet",caps!=null && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET))
            .put("validated",caps!=null && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED))
            .put("roaming",caps==null || !caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_ROAMING))
            .put("suspended",caps==null || !caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_SUSPENDED))
            .put("congested",caps==null || !caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_CONGESTED));
        JSONArray transports=new JSONArray(),dns=new JSONArray();
        if(caps!=null)for(int t=0;t<=7;t++)if(caps.hasTransport(t))transports.put(t);
        LinkProperties link=active==null?null:manager.getLinkProperties(active);
        if(link!=null)for(InetAddress address:link.getDnsServers())dns.put(address.getHostAddress());
        result.put("transports",transports).put("dns",dns);
        // Additive topology for 12L's shared-uplink NetworkAgent. Keep the
        // existing INFO response available on older hosts lacking an accessor.
        if(link!=null)try {
            JSONArray addresses=new JSONArray(),routes=new JSONArray();
            for(LinkAddress address:link.getLinkAddresses())addresses.put(address.toString());
            java.lang.reflect.Method routeType=RouteInfo.class.getMethod("getType");
            for(RouteInfo route:link.getRoutes()) {
                InetAddress gateway=route.getGateway();
                routes.put(new JSONObject().put("destination",route.getDestination().toString())
                    .put("gateway",gateway==null?"":gateway.getHostAddress()).put("type",routeType.invoke(route)));
            }
            result.put("addresses",addresses).put("routes",routes).put("mtu",link.getMtu());
        }catch(ReflectiveOperationException | LinkageError unavailable) {
            result.put("topology_error",unavailable.getClass().getSimpleName());
        }
        return result;
    }
    private void accept() {
        while(!closed) {
            LocalSocket peer=null;
            try {
                peer=server.accept();
                if(peer.getPeerCredentials().getUid()!=android.os.Process.myUid()){peer.close();continue;}
                clients.add(peer);final LocalSocket accepted=peer;workers.execute(()->serve(accepted));
            }catch(Exception error){
                if(peer!=null){clients.remove(peer);try{peer.close();}catch(IOException ignored){}}
                if(!closed)android.util.Log.w("PocketHostDns","DNS peer unavailable: "+error.getClass().getSimpleName());
            }
        }
    }
    private void serve(LocalSocket peer) {
        try(LocalSocket socket=peer) {
            socket.setSoTimeout(15000);
            DataInputStream in=new DataInputStream(socket.getInputStream());DataOutputStream out=new DataOutputStream(socket.getOutputStream());
            if(in.readInt()!=HostWire.MAGIC)return;
            int operation=in.readInt();byte[] request=HostWire.read(in,16384),response;int status=0;
            try {
                if(closed)throw new IOException("DNS bridge closed");
                if((operation==HostWire.DNS || operation==HostWire.DNS_SERVERS || operation==HostWire.DNS_PACKET) && !trustedPeer(root,socket.getPeerCredentials().getPid()))
                    throw new SecurityException("Use the guest resolver service");
                if(operation==HostWire.DNS)response=resolve(request);
                else if(operation==HostWire.DNS_PACKET)response=HostDnsPacket.resolve(context,request);
                else if(operation==HostWire.DNS_SERVERS) {
                    if(request.length!=0)throw new IllegalArgumentException("DNS server query has no payload");
                    response=servers(context);
                }
                else if(operation==HostWire.INFO)response=new JSONObject().put("protocol",1).put("network",network(context)).toString().getBytes(StandardCharsets.UTF_8);
                else {status=404;response=new byte[0];}
            }catch(SecurityException denied){status=403;response=new byte[0];}
            catch(IllegalArgumentException invalid){status=400;response=new byte[0];}
            catch(UnsupportedOperationException unavailable){status=404;response=new byte[0];}
            catch(java.net.SocketTimeoutException | java.util.concurrent.TimeoutException timeout){status=504;response=new byte[0];}
            catch(Exception unavailable){status=503;response=new byte[0];}
            out.writeInt(status);HostWire.write(out,response);out.flush();
        }catch(IOException ignored){}finally{clients.remove(peer);}
    }
    @Override public void close() {
        closed=true;try{server.close();}catch(IOException ignored){}
        try{bound.close();}catch(IOException ignored){}
        for(LocalSocket peer:clients)try{peer.close();}catch(IOException ignored){}
        workers.shutdownNow();endpoint.delete();
    }
}
