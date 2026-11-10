// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.net.*;
import dev.kreijstal.aosppocket.bridge.HostWire;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.Set;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicLong;
import org.json.*;

/**
 * Host audio endpoint for the 12L runtime and its validation fixture: guest playback (AUDIO_OUT) and, when
 * this app holds RECORD_AUDIO, host microphone capture (AUDIO_IN) for the guest audio HAL's input stream.
 * Only the guest's audioserver may open either stream; without the permission the HAL reads silence.
 */
public final class HostAudio implements AutoCloseable {
    private final File endpoint,root;
    private final android.content.Context context;
    private final LocalSocket bound=new LocalSocket();
    private final LocalServerSocket server;
    private final Set<LocalSocket> clients=ConcurrentHashMap.newKeySet();
    private final ThreadPoolExecutor workers=new ThreadPoolExecutor(4,4,0,TimeUnit.SECONDS,new ArrayBlockingQueue<>(4));
    private final AtomicLong bytes=new AtomicLong(),completed=new AtomicLong(),errors=new AtomicLong(),
        microphoneStreams=new AtomicLong(),microphoneDenied=new AtomicLong(),microphonePeerClosures=new AtomicLong();
    private volatile String lastStreamErrorClass;
    private volatile boolean closed;
    public HostAudio(File root)throws IOException {this(root,null);}
    /** With a context, AUDIO_IN streams the host microphone; without one, input is refused. */
    public HostAudio(File root,android.content.Context context)throws IOException {
        this.root=root;this.context=context;endpoint=new File(root,"dev/socket/pocket_audio");
        if(endpoint.exists() && !endpoint.delete())throw new IOException("Cannot reset audio endpoint");
        bound.bind(new LocalSocketAddress(endpoint.getPath(),LocalSocketAddress.Namespace.FILESYSTEM));
        server=new LocalServerSocket(bound.getFileDescriptor());
        new Thread(this::accept,"PocketHostAudio32").start();
    }
    private void accept() {
        while(!closed) {
            LocalSocket peer=null;
            try {
                peer=server.accept();
                if(peer.getPeerCredentials().getUid()!=android.os.Process.myUid()) {peer.close();continue;}
                clients.add(peer);final LocalSocket accepted=peer;workers.execute(()->serve(accepted));
            }catch(Exception error) {
                if(peer!=null){clients.remove(peer);try{peer.close();}catch(IOException ignored){}}
                if(!closed)errors.incrementAndGet();
            }
        }
    }
    private void serve(LocalSocket peer) {
        try(LocalSocket socket=peer) {
            socket.setSoTimeout(10000);
            DataInputStream in=new DataInputStream(socket.getInputStream());
            DataOutputStream out=new DataOutputStream(socket.getOutputStream());
            if(in.readInt()!=HostWire.MAGIC)return;
            int op=in.readInt();byte[] request=HostWire.read(in,16384);
            if(op==HostWire.AUDIO_IN && HostDns.trustedPeer(root,socket.getPeerCredentials().getPid(),1041)) {
                if(context==null || context.checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)
                        !=android.content.pm.PackageManager.PERMISSION_GRANTED) {
                    microphoneDenied.incrementAndGet();
                    out.writeInt(403);HostWire.write(out,"Host permission required: android.permission.RECORD_AUDIO".getBytes(StandardCharsets.UTF_8));
                    out.flush();return;
                }
                microphoneStreams.incrementAndGet();
                try {HostMedia.capture(socket,new JSONObject(new String(request,StandardCharsets.UTF_8)),out,()->microphonePeerClosures.incrementAndGet());}
                catch(IllegalArgumentException invalid){out.writeInt(400);HostWire.write(out,"Invalid guest audio input request".getBytes(StandardCharsets.UTF_8));out.flush();return;}
                catch(IOException ended){if(ended instanceof java.net.SocketTimeoutException || closed)return;throw ended;}
                catch(UnsupportedOperationException unavailable){out.writeInt(503);HostWire.write(out,unavailable.getMessage().getBytes(StandardCharsets.UTF_8));out.flush();return;}
                completed.incrementAndGet();return;
            }
            if(op!=HostWire.AUDIO_OUT || !HostDns.trustedPeer(root,socket.getPeerCredentials().getPid(),1000,1041)) {
                out.writeInt(403);HostWire.write(out,new byte[0]);out.flush();return;
            }
            HostMedia.stream(socket,new JSONObject(new String(request,StandardCharsets.UTF_8)),in,out,
                count->bytes.addAndGet(count));
            completed.incrementAndGet();
        }catch(Exception error){if(!closed){lastStreamErrorClass=error.getClass().getName();errors.incrementAndGet();}}
        finally{clients.remove(peer);}
    }
    public JSONObject report()throws JSONException {
        return new JSONObject().put("hostPcmBytesWritten",bytes.get()).put("completedStreams",completed.get())
            .put("streamErrors",errors.get()).put("microphoneRequested",microphoneStreams.get()+microphoneDenied.get()>0)
            .put("microphoneStreams",microphoneStreams.get()).put("microphoneDenied",microphoneDenied.get())
            .put("microphonePeerClosures",microphonePeerClosures.get())
            .put("lastStreamErrorClass",lastStreamErrorClass==null?JSONObject.NULL:lastStreamErrorClass);
    }
    @Override public void close() {
        closed=true;try{server.close();}catch(IOException ignored){}
        try{bound.close();}catch(IOException ignored){}
        for(LocalSocket peer:clients)try{peer.close();}catch(IOException ignored){}
        workers.shutdownNow();endpoint.delete();
    }
}
