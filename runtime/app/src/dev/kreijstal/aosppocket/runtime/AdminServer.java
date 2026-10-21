// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.security.*;
import java.util.*;
import java.util.concurrent.*;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/** Explicit owner access. Loopback only; never an unauthenticated guest socket relay. */
final class AdminServer implements AutoCloseable {
    interface Backend {
        String status() throws Exception;
        String control(boolean start) throws Exception;
        Process execute(String cwd, List<String> argv) throws Exception;
    }
    static final byte[] MAGIC="PKADMIN1".getBytes(StandardCharsets.US_ASCII);
    static final int DEFAULT_PORT=2228, CHUNK=65536;
    private final ServerSocket server;
    private final byte[] key;
    private final Backend backend;
    private final Set<Socket> clients=ConcurrentHashMap.newKeySet();
    private final Set<Process> processes=ConcurrentHashMap.newKeySet();
    private final ThreadPoolExecutor workers=new ThreadPoolExecutor(0,4,30,TimeUnit.SECONDS,new SynchronousQueue<Runnable>());
    private volatile boolean closed;

    AdminServer(int port, byte[] key, Backend backend) throws IOException {
        if(key.length!=32)throw new IllegalArgumentException("Expected 32-byte owner key");
        this.key=key.clone();this.backend=backend;
        server=new ServerSocket();
        try {server.bind(new InetSocketAddress(InetAddress.getByName("127.0.0.1"),port),4);}
        catch(IOException error){server.close();throw error;}
        Thread acceptor=new Thread(this::accept,"PocketAdminAccept");acceptor.setDaemon(true);acceptor.start();
    }
    int port(){return server.getLocalPort();}
    private void accept() {
        while(!closed) {
            Socket client=null;
            try {
                client=server.accept();client.setSoTimeout(5000);client.setTcpNoDelay(true);clients.add(client);
                final Socket accepted=client;workers.execute(()->serve(accepted));
            } catch(IOException | RejectedExecutionException error) {
                if(client!=null){clients.remove(client);try{client.close();}catch(IOException ignored){}}
            }
        }
    }
    static byte[] response(byte[] key,byte[] nonce) throws GeneralSecurityException {
        Mac mac=Mac.getInstance("HmacSHA256");mac.init(new SecretKeySpec(key,"HmacSHA256"));
        mac.update(MAGIC);return mac.doFinal(nonce);
    }
    private static String string(DataInputStream in,int max) throws IOException {
        int size=in.readInt();if(size<0 || size>max)throw new IOException("Invalid argument length");
        byte[] bytes=new byte[size];in.readFully(bytes);
        String value=new String(bytes,StandardCharsets.UTF_8);
        if(value.indexOf(0)>=0)throw new IOException("NUL in argument");return value;
    }
    private static void frame(DataOutputStream out,int type,byte[] data,int size) throws IOException {
        synchronized(out){out.writeByte(type);out.writeInt(size);out.write(data,0,size);out.flush();}
    }
    private static void text(DataOutputStream out,int type,String value) throws IOException {
        byte[] data=value.getBytes(StandardCharsets.UTF_8);frame(out,type,data,data.length);
    }
    private static void exit(DataOutputStream out,int code) throws IOException {
        synchronized(out){out.writeByte(3);out.writeInt(4);out.writeInt(code);out.flush();}
    }
    private static Thread pump(String name,Runnable work) {
        Thread thread=new Thread(work,name);thread.setDaemon(true);thread.start();return thread;
    }
    private static Thread output(DataOutputStream out,InputStream source,int type,Socket client) {
        return pump("PocketAdminOutput",()->{
            try(InputStream stream=source){byte[] bytes=new byte[CHUNK];int n;
                while((n=stream.read(bytes))!=-1)frame(out,type,bytes,n);
            } catch(IOException disconnected){try{client.close();}catch(IOException ignored){}}
        });
    }
    private void serve(Socket client) {
        Process process=null;
        try(Socket peer=client) {
            DataInputStream in=new DataInputStream(new BufferedInputStream(peer.getInputStream()));
            DataOutputStream out=new DataOutputStream(new BufferedOutputStream(peer.getOutputStream()));
            byte[] nonce=new byte[32],proof=new byte[32];new SecureRandom().nextBytes(nonce);
            out.write(MAGIC);out.write(nonce);out.flush();in.readFully(proof);
            boolean authorized=MessageDigest.isEqual(proof,response(key,nonce));
            out.writeByte(authorized?1:0);out.flush();if(!authorized)return;
            byte[] session=new byte[64];System.arraycopy(nonce,0,session,0,32);System.arraycopy(proof,0,session,32,32);
            out.write(response(key,session));out.flush();
            try {
                int operation=in.readUnsignedByte();
                if(operation==1 || operation==3 || operation==4) {
                    String result=operation==1?backend.status():backend.control(operation==3);
                    text(out,1,result+"\n");exit(out,0);return;
                }
                if(operation!=2)throw new IOException("Unknown admin operation");
                String cwd=string(in,4096);if(!cwd.startsWith("/"))throw new IOException("Guest working directory must be absolute");
                int count=in.readInt();if(count<1 || count>128)throw new IOException("Invalid argument count");
                List<String> argv=new ArrayList<>();int total=0;
                for(int i=0;i<count;i++){String arg=string(in,CHUNK);total+=arg.length();if(total>262144)throw new IOException("Command too large");argv.add(arg);}
                if(argv.get(0).isEmpty())throw new IOException("Empty executable");
                process=backend.execute(cwd,Collections.unmodifiableList(argv));processes.add(process);
                if(closed || client.isClosed()){process.destroyForcibly();return;}
                peer.setSoTimeout(0);
                final Process running=process;
                Thread stdout=output(out,process.getInputStream(),1,peer),stderr=output(out,process.getErrorStream(),2,peer);
                Thread input=pump("PocketAdminInput",()->{
                    try(OutputStream stream=running.getOutputStream()) {
                        byte[] bytes=new byte[CHUNK];boolean writable=true;
                        while(true){int type=in.readUnsignedByte(),size=in.readInt();
                            if(type==2 && size==0){
                                try{stream.close();}catch(IOException stdinClosed){/* Command already closed stdin. */}
                                // Keep watching the connection after stdin EOF, so Ctrl-C
                                // or a disconnected client also stops a silent command.
                                in.read();running.destroyForcibly();return;
                            }
                            if(type!=1 || size<1 || size>CHUNK)throw new IOException("Invalid stdin frame");
                            in.readFully(bytes,0,size);
                            if(writable)try{stream.write(bytes,0,size);stream.flush();}
                            catch(IOException stdinClosed){writable=false;}
                        }
                    } catch(IOException disconnected){running.destroyForcibly();}
                });
                int code;
                while(!process.waitFor(200,TimeUnit.MILLISECONDS)) {
                    if(closed || peer.isClosed())process.destroyForcibly();
                }
                code=process.exitValue();stdout.join(5000);stderr.join(5000);
                if(stdout.isAlive() || stderr.isAlive())throw new IOException("Command descendants kept output open");
                exit(out,code);
                // Closing peer below also releases a stdin worker waiting on the client.
                input.interrupt();
            } catch(Exception error) {
                text(out,4,error.getMessage()==null?error.getClass().getSimpleName():error.getMessage());exit(out,1);
            }
        } catch(IOException | GeneralSecurityException disconnected) { /* No secrets or commands in logs. */ }
        finally {
            clients.remove(client);
            if(process!=null){processes.remove(process);if(process.isAlive())process.destroyForcibly();}
        }
    }
    @Override public void close() {
        closed=true;try{server.close();}catch(IOException ignored){}
        for(Socket client:clients)try{client.close();}catch(IOException ignored){}
        for(Process process:processes)process.destroyForcibly();
        workers.shutdownNow();
    }
}
