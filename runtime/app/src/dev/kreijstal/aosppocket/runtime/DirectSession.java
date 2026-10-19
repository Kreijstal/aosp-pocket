// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.content.Context;
import android.graphics.*;
import android.net.*;
import android.os.*;
import android.view.MotionEvent;
import java.io.*;
import java.util.Set;
import java.util.concurrent.*;
import dev.kreijstal.aosppocket.bridge.DisplayGeometry;
import dev.kreijstal.aosppocket.bridge.AnonymousFrames;
import android.view.Surface;

/** Persistent Android 9 session owned by GuestService, independent of windows. */
final class DirectSession implements AutoCloseable {
    interface Listener {
        void status(String message);
        void ready();
        void stopped(String message);
    }
    private final Context context;
    private final Listener listener;
    private final DisplayGeometry geometry;
    private final Handler main=new Handler(Looper.getMainLooper());
    private final Set<LocalSocket> sockets=ConcurrentHashMap.newKeySet();
    private final ThreadPoolExecutor commands=new ThreadPoolExecutor(1,1,0,TimeUnit.SECONDS,new ArrayBlockingQueue<>(128));
    private final Thread worker;
    private volatile boolean closed,visible,ready;
    private volatile java.lang.Process init;
    private BootBattery battery;
    private HostBroker hostBroker;
    private File root;
    private Connection input;
    private final Object frameLock=new Object(), visibilityLock=new Object();
    private volatile Surface surface;
    private AnonymousFrames sharedFrames;
    private int front=-1;
    void surface(Surface value) {synchronized(frameLock){surface=value;}wakeDisplay();}
    void detach(Surface value) {synchronized(frameLock){if(surface==value)surface=null;}wakeDisplay();}
    private void wakeDisplay(){synchronized(visibilityLock){visibilityLock.notifyAll();}}
    Bitmap snapshot() {
        synchronized(frameLock) {
            if(sharedFrames==null || front<0)return null;
            Bitmap bitmap=Bitmap.createBitmap(geometry.width,geometry.height,Bitmap.Config.ARGB_8888);
            bitmap.copyPixelsFromBuffer(sharedFrames.slot(front));return bitmap;
        }
    }
    DirectSession(Context context,DisplayGeometry geometry,Listener listener) {
        this.context=context;this.geometry=geometry;this.listener=listener;worker=new Thread(this::run,"PocketAndroid9");
    }
    void start(){worker.start();}
    boolean running(){return init!=null && init.isAlive() && !closed;}
    void visible(boolean value){visible=value;wakeDisplay();}
    private void status(String text){main.post(()->{if(!closed)listener.status(text);});}
    private void run() {
        String ended="Android 9 stopped. Tap Start to boot again.";
        try {
            final int[] percent={-1};
            root=DirectImage.prepare(context,geometry,(done,total)->{
                int value=total==0?0:(int)(100*done/total);
                if(value!=percent[0]){percent[0]=value;status("Setting up Android 9: "+value+"%");}
            });
            synchronized(this) {
                if(closed)return;
                java.nio.file.Files.deleteIfExists(new File(root,"logs/display-performance.json").toPath());
                battery=new BootBattery(context,root);hostBroker=new HostBroker(context,root);init=DirectImage.start(root);
            }
            status("Android 9 starting…");
            long deadline=SystemClock.elapsedRealtime()+240000;
            Connection frames=null;
            while(!closed && init.isAlive() && SystemClock.elapsedRealtime()<deadline) {
                try {frames=new Connection();break;}
                catch(IOException notReady){Thread.sleep(300);}
            }
            if(frames==null)throw new IOException("Android 9 did not open its display/input endpoint; see api28-rootfs/logs/manager-init.log");
            AnonymousFrames shared=AnonymousFrames.create(geometry);
            try(Connection channel=frames) {
                channel.socket.setFileDescriptorsForSend(new FileDescriptor[]{shared.descriptor()});
                channel.out.writeInt(11);channel.out.flush();
                channel.socket.setFileDescriptorsForSend(null);
                if(!channel.in.readBoolean())throw new IOException("Guest refused shared display memory");
                synchronized(frameLock){sharedFrames=shared;front=-1;}
                ready=true;main.post(()->{if(!closed)listener.ready();});
                long sequence=0,framesPresented=0,intervalFrames=0;
                long measuredAt=SystemClock.elapsedRealtimeNanos(),captureTotal=0,presentTotal=0,maxCapture=0;
                while(!closed && init.isAlive()) {
                    boolean waited=false;
                    synchronized(visibilityLock) {
                        while(!closed && init.isAlive() && (!visible || surface==null || !surface.isValid())){waited=true;visibilityLock.wait(1000);}
                    }
                    if(waited){measuredAt=SystemClock.elapsedRealtimeNanos();intervalFrames=0;}
                    if(closed || !init.isAlive())break;
                    long began=SystemClock.elapsedRealtimeNanos();int slot=(int)(sequence&1);sequence++;
                    channel.out.writeInt(10);channel.out.writeInt(slot);channel.out.writeLong(sequence);channel.out.flush();
                    if(channel.in.readLong()!=sequence || channel.in.readInt()!=slot)throw new IOException("Shared frame ownership mismatch");
                    long capture=channel.in.readLong();if(capture<0)throw new IOException("Invalid capture duration");
                    long presentBegan=SystemClock.elapsedRealtimeNanos();boolean presented=false;
                    synchronized(frameLock) {
                        front=slot;
                        Surface target=surface;
                        if(!closed && visible && target!=null && target.isValid())
                            presented=Native.presentFrame(target,shared.slot(slot),geometry.width,geometry.height);
                    }
                    if(presented){framesPresented++;intervalFrames++;}
                    captureTotal+=capture;maxCapture=Math.max(maxCapture,capture);
                    presentTotal+=SystemClock.elapsedRealtimeNanos()-presentBegan;
                    long now=SystemClock.elapsedRealtimeNanos(),elapsed=now-measuredAt;
                    if(elapsed>=2000000000L) {
                        org.json.JSONObject report=new org.json.JSONObject()
                            .put("transport","shared-rgba").put("backing","anonymous").put("codec","none")
                            .put("pixel_bytes_over_socket",0).put("target_fps",60)
                            .put("width",geometry.width).put("height",geometry.height).put("shared_bytes",2L*shared.frameBytes)
                            .put("frames_presented",framesPresented).put("frames_requested",sequence)
                            .put("interval_fps",intervalFrames*1000000000.0/elapsed)
                            .put("mean_capture_ms",captureTotal/sequence/1000000.0)
                            .put("max_capture_ms",maxCapture/1000000.0)
                            .put("mean_present_ms",presentTotal/sequence/1000000.0);
                        java.nio.file.Files.write(new File(root,"logs/display-performance.json").toPath(),report.toString().getBytes("UTF-8"));
                        android.util.Log.i("PocketFrames",report.toString());
                        measuredAt=now;intervalFrames=0;
                    }
                    // Pace the producer without building a stale frame or UI callback queue.
                    long remaining=16666667L-(SystemClock.elapsedRealtimeNanos()-began);
                    if(remaining>0)Thread.sleep(remaining/1000000L,(int)(remaining%1000000L));
                }
            } finally {synchronized(frameLock){sharedFrames=null;front=-1;shared.close();}}

            if(!closed)ended="Android 9 exited unexpectedly. See api28-rootfs/logs/manager-init.log.";
        } catch(InterruptedException cancelled){Thread.currentThread().interrupt();}
        catch(Exception error){if(!closed)ended="Android 9 failed: "+error.getMessage();}
        finally {
            closed=true;ready=false;commands.shutdownNow();
            for(LocalSocket socket:sockets)try{socket.close();}catch(IOException ignored){}
            java.lang.Process current=init;
            if(current!=null) {
                current.destroy();
                try {if(!current.waitFor(8,TimeUnit.SECONDS)){current.destroyForcibly();current.waitFor(2,TimeUnit.SECONDS);}}
                catch(InterruptedException interrupted){current.destroyForcibly();}
            }
            if(battery!=null)battery.close();
            if(hostBroker!=null)hostBroker.close();
            final String message=ended;main.post(()->listener.stopped(message));
        }
    }
    private final class Connection implements AutoCloseable {
        final LocalSocket socket=new LocalSocket();
        final DataInputStream in;
        final DataOutputStream out;
        Connection() throws IOException {
            sockets.add(socket);
            try {
                if(closed)throw new IOException("Session closed");
                socket.connect(new LocalSocketAddress(new File(root,"dev/socket/pocket_control").getPath(),LocalSocketAddress.Namespace.FILESYSTEM));
                socket.setSoTimeout(15000);
                in=new DataInputStream(new BufferedInputStream(socket.getInputStream()));
                out=new DataOutputStream(new BufferedOutputStream(socket.getOutputStream()));
                DisplayGeometry actual=DisplayGeometry.read(in);
                if(!geometry.equals(actual))throw new IOException("Guest display "+actual+" does not match host "+geometry);
            } catch(IOException error){close();throw error;}
        }
        @Override public void close(){sockets.remove(socket);try{socket.close();}catch(IOException ignored){}}
    }
    private interface Command {void run(Connection connection)throws Exception;}
    private void submit(Command command) {
        if(!ready || closed)return;
        try {commands.execute(()->{
            try {if(input==null)input=new Connection();command.run(input);}
            catch(Exception error){if(input!=null)input.close();input=null;status("Android 9 control failed: "+error.getMessage());}
        });}catch(RejectedExecutionException full){status("Guest input queue is busy; try again.");}
    }
    void touch(MotionEvent event,float scaleX,float scaleY) {
        if(event.getPointerCount()>10)return;
        final int count=event.getPointerCount(),action=event.getAction();
        final long down=event.getDownTime(),time=event.getEventTime();
        final int[] ids=new int[count];final float[] x=new float[count],y=new float[count];
        for(int i=0;i<count;i++){ids[i]=event.getPointerId(i);x[i]=event.getX(i)*scaleX;y[i]=event.getY(i)*scaleY;}
        submit(c->{c.out.writeInt(2);c.out.writeLong(down);c.out.writeLong(time);c.out.writeInt(action);c.out.writeInt(count);
            for(int i=0;i<count;i++){c.out.writeInt(ids[i]);c.out.writeFloat(x[i]);c.out.writeFloat(y[i]);}
            c.out.flush();if(!c.in.readBoolean())throw new IOException("Guest rejected touch input");});
    }
    void key(int code){submit(c->{c.out.writeInt(3);c.out.writeInt(code);c.out.flush();if(!c.in.readBoolean())throw new IOException("Guest rejected navigation");});}
    void importHostContacts(){submit(c->{c.socket.setSoTimeout(90000);try{c.out.writeInt(5);c.out.flush();status(c.in.readUTF());}finally{c.socket.setSoTimeout(15000);}});}
    void checkHostBridges(){submit(c->{
        c.socket.setSoTimeout(30000);
        try {
            c.out.writeInt(6);c.out.writeUTF("https://example.com/");c.out.flush();String result=c.in.readUTF();
            java.nio.file.Files.write(new File(root,"logs/host-bridge-check.json").toPath(),result.getBytes("UTF-8"));
            org.json.JSONObject report=new org.json.JSONObject(result);
            status("Guest network: "+(report.optBoolean("guest_network_connected")?"connected":"offline")+
                " · HTTPS "+(report.has("http_status")?report.optInt("http_status"):report.optString("http_error","unavailable")));
        } finally {c.socket.setSoTimeout(15000);}
    });}
    void checkHostDevices(){submit(c->{
        c.socket.setSoTimeout(60000);
        try {
            c.out.writeInt(7);c.out.flush();String result=c.in.readUTF();
            java.nio.file.Files.write(new File(root,"logs/host-device-check.json").toPath(),result.getBytes("UTF-8"));
            org.json.JSONObject report=new org.json.JSONObject(result);StringBuilder summary=new StringBuilder("Host checks:");
            for(String name:new String[]{"camera","audio","sensors","contacts","phone","location"})
                summary.append(' ').append(name).append('=').append(report.getJSONObject(name).getString("status"));
            status(summary.toString());
        } finally {c.socket.setSoTimeout(15000);}
    });}
    void openGuestDialer(){submit(c->{c.out.writeInt(8);c.out.flush();if(!c.in.readBoolean())throw new IOException("Guest dialer did not open");});}
    boolean hasLastApp(){return !context.getSharedPreferences("guest-last-app",Context.MODE_PRIVATE).getString("package","").isEmpty();}
    void openLastApp(){submit(c->{
        String name=context.getSharedPreferences("guest-last-app",Context.MODE_PRIVATE).getString("package","");
        if(name.isEmpty())throw new IOException("Install a guest APK first");
        c.out.writeInt(9);c.out.writeUTF(name);c.out.flush();
        if(!c.in.readBoolean())throw new IOException("App did not open");
    });}
    void install(android.net.Uri uri) {
        submit(c->{
            status("Installing APK in Android 9…");
            File target=new File(root,"system-data/pocket-import.apk"),temp=new File(target.getPath()+".new");
            try {
                try(InputStream in=context.getContentResolver().openInputStream(uri);OutputStream out=new FileOutputStream(temp)) {
                    if(in==null)throw new IOException("Cannot open selected APK");GuestImage.copy(in,out,512L*1024*1024);
                }
                java.nio.file.Files.move(temp.toPath(),target.toPath(),java.nio.file.StandardCopyOption.ATOMIC_MOVE,java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                c.socket.setSoTimeout(90000);c.out.writeInt(4);c.out.flush();
                String result=c.in.readUTF();
                if(result.startsWith("Installed "))context.getSharedPreferences("guest-last-app",Context.MODE_PRIVATE)
                    .edit().putString("package",result.substring(10)).apply();
                status(result);
            } finally {temp.delete();target.delete();c.socket.setSoTimeout(15000);}
        });
    }
    @Override public synchronized void close() {
        closed=true;ready=false;wakeDisplay();commands.shutdownNow();
        for(LocalSocket socket:sockets)try{socket.close();}catch(IOException ignored){}
        if(init!=null)init.destroy();
        // Do not interrupt waitFor during supervised cleanup: init needs time
        // to stop and reap its services before a new session can start.
        if(init==null)worker.interrupt();
    }
}
