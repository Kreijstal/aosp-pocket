// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.content.Context;
import android.net.*;
import android.os.Build;
import android.os.CancellationSignal;
import java.io.*;
import java.net.*;
import java.nio.*;
import java.util.Arrays;
import java.util.concurrent.*;
import java.security.SecureRandom;

/** Bounded real DNS packets through the host resolver or explicit local servers. */
final class HostDnsPacket {
    private static final int MAX=8192;
    private static final SecureRandom random=new SecureRandom();
    private static final ExecutorService callbacks=Executors.newFixedThreadPool(2,r->{Thread t=new Thread(r,"PocketDnsAnswer");t.setDaemon(true);return t;});
    private static int word(byte[] packet,int offset){return ((packet[offset]&255)<<8)|(packet[offset+1]&255);}
    static void validateQuery(byte[] packet) {
        if(packet.length<17 || packet.length>MAX || (packet[2]&0xf8)!=0 || (packet[3]&15)!=0 || word(packet,4)!=1
                || word(packet,6)!=0 || word(packet,8)!=0)throw new IllegalArgumentException("Invalid DNS query");
        // Queries from libresolv contain one ordinary, uncompressed question.
        int offset=12,length=0;
        while(true) {
            if(offset>=packet.length)throw new IllegalArgumentException("Truncated DNS question");
            int size=packet[offset++]&255;if(size==0)break;
            if(size>63 || offset+size>packet.length || (length+=size+1)>254)throw new IllegalArgumentException("Invalid DNS label");
            offset+=size;
        }
        if(offset+4>packet.length || word(packet,offset)==0 || word(packet,offset+2)==0)
            throw new IllegalArgumentException("Invalid DNS question type/class");
    }
    private static byte[] question(byte[] packet)throws IOException {
        ByteArrayOutputStream name=new ByteArrayOutputStream();int offset=12,end=-1,length=0,hops=0;
        while(true) {
            if(offset>=packet.length || ++hops>128)throw new IOException("Invalid DNS question name");
            int size=packet[offset++]&255;
            if((size&0xc0)==0xc0) {
                if(offset>=packet.length)throw new IOException("Truncated DNS name pointer");
                int target=((size&63)<<8)|(packet[offset++]&255);
                if(target>=offset-2)throw new IOException("Invalid DNS name pointer");
                if(end<0)end=offset;offset=target;continue;
            }
            if(size>63 || offset+size>packet.length || (length+=size+1)>255)throw new IOException("Invalid DNS question label");
            name.write(size);if(size==0)break;
            for(int i=0;i<size;i++) {int c=packet[offset++]&255;name.write(c>='A' && c<='Z'?c+32:c);}
        }
        if(end>=0)offset=end;
        if(offset+4>packet.length)throw new IOException("Truncated DNS question type/class");
        name.write(packet,offset,4);return name.toByteArray();
    }
    static void validateAnswer(byte[] query,byte[] answer)throws IOException {
        if(answer==null || answer.length<12 || answer.length>MAX || word(query,0)!=word(answer,0)
                || (answer[2]&0xf8)!=0x80 || word(answer,4)!=1
                || !Arrays.equals(question(query),question(answer)))throw new IOException("Invalid DNS response");
    }
    private static byte[] response(byte[] query,byte[] answer,int rcode)throws IOException {
        validateAnswer(query,answer);
        if(rcode<0 || rcode>4095)throw new IOException("Invalid DNS response code");
        return ByteBuffer.allocate(4+answer.length).order(ByteOrder.BIG_ENDIAN).putInt(rcode).put(answer).array();
    }
    static byte[] resolve(Context context,byte[] request)throws Exception {
        if(request.length<8+17 || request.length>8+MAX)throw new IllegalArgumentException("Invalid DNS packet request");
        ByteBuffer header=ByteBuffer.wrap(request).order(ByteOrder.BIG_ENDIAN);
        int flags=header.getInt(),bypass=header.getInt();
        if((flags&~7)!=0 || (bypass!=0 && bypass!=1))throw new IllegalArgumentException("Invalid DNS flags");
        byte[] query=Arrays.copyOfRange(request,8,request.length);validateQuery(query);
        ConnectivityManager manager=context.getSystemService(ConnectivityManager.class);Network network=manager.getActiveNetwork();
        if(network==null)throw new IOException("Host has no default network");
        if(bypass==0 && Build.VERSION.SDK_INT>=29)return Api29.query(network,query,flags);
        LinkProperties properties=manager.getLinkProperties(network);
        if(properties==null || properties.getDnsServers().isEmpty())throw new IOException("Host has no DNS servers");
        // A pre-29 host has no public raw resolver API for private DNS. Never
        // silently turn an ordinary private-DNS request into plaintext UDP.
        if(bypass==0 && properties.isPrivateDnsActive())throw new UnsupportedOperationException("Raw private DNS requires the host resolver API");
        IOException last=null;
        int servers=0;
        for(InetAddress server:properties.getDnsServers()) {
            if(servers++==2)break;
            try(DatagramSocket socket=new DatagramSocket()) {
                byte[] wire=query.clone();int id=random.nextInt(65536);wire[0]=(byte)(id>>8);wire[1]=(byte)id;
                network.bindSocket(socket);socket.connect(server,53);socket.setSoTimeout(2500);
                socket.send(new DatagramPacket(wire,wire.length));
                byte[] bytes=new byte[MAX+1];DatagramPacket packet=new DatagramPacket(bytes,bytes.length);socket.receive(packet);
                if(packet.getLength()>MAX)throw new IOException("DNS response exceeds bound");
                byte[] answer=Arrays.copyOf(bytes,packet.getLength());validateAnswer(wire,answer);
                if((answer[2]&2)!=0)answer=tcp(network,server,wire);
                answer[0]=query[0];answer[1]=query[1];
                return response(query,answer,answer[3]&15);
            }catch(IOException failed){last=failed;}
        }
        throw last==null?new IOException("DNS servers unavailable"):last;
    }
    private static byte[] tcp(Network network,InetAddress server,byte[] query)throws IOException {
        try(Socket socket=new Socket()) {
            network.bindSocket(socket);socket.connect(new InetSocketAddress(server,53),2500);socket.setSoTimeout(2500);
            DataOutputStream out=new DataOutputStream(socket.getOutputStream());out.writeShort(query.length);out.write(query);out.flush();
            DataInputStream in=new DataInputStream(socket.getInputStream());int length=in.readUnsignedShort();
            if(length<12 || length>MAX)throw new IOException("TCP DNS response exceeds bound");
            byte[] answer=new byte[length];in.readFully(answer);validateAnswer(query,answer);return answer;
        }
    }
    private static final class Api29 {
        static byte[] query(Network network,byte[] query,int flags)throws Exception {
            CompletableFuture<byte[]> answer=new CompletableFuture<>();CancellationSignal cancel=new CancellationSignal();
            android.net.DnsResolver.getInstance().rawQuery(network,query,flags,callbacks,cancel,new android.net.DnsResolver.Callback<byte[]>() {
                @Override public void onAnswer(byte[] bytes,int rcode) {
                    try{answer.complete(response(query,bytes,rcode));}catch(IOException error){answer.completeExceptionally(error);}
                }
                @Override public void onError(android.net.DnsResolver.DnsException error){
                    Throwable cause=error.getCause();
                    answer.completeExceptionally(cause instanceof android.system.ErrnoException
                        && ((android.system.ErrnoException)cause).errno==android.system.OsConstants.ETIMEDOUT
                        ?new SocketTimeoutException("Host DNS timed out"):error);
                }
            });
            try{return answer.get(8,TimeUnit.SECONDS);}
            catch(ExecutionException failed){Throwable cause=failed.getCause();if(cause instanceof Exception)throw (Exception)cause;throw new IOException(cause);}
            finally{cancel.cancel();}
        }
    }
}
