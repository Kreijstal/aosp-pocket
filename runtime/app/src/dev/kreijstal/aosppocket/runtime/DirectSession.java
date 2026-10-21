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
    // APK copies and PackageInstaller callbacks can take minutes. Input must
    // keep its ordered socket and worker while those operations are pending.
    private final ThreadPoolExecutor operations=new ThreadPoolExecutor(1,1,0,TimeUnit.SECONDS,new ArrayBlockingQueue<>(8));
    private final Thread worker;
    private volatile boolean closed,visible,ready;
    private volatile java.lang.Process init;
    private BootBattery battery;
    private HostBroker hostBroker;
    private RuntimeLog runtimeLog;
    private File root;
    private Connection input;
    private final Object frameLock=new Object(), visibilityLock=new Object();
    private volatile Surface surface;
    private long surfaceGeneration;
    private AnonymousFrames sharedFrames;
    private int front=-1;
    void surface(Surface value) {synchronized(frameLock){surface=value;surfaceGeneration++;}wakeDisplay();}
    void detach(Surface value) {synchronized(frameLock){if(surface==value){surface=null;surfaceGeneration++;}}wakeDisplay();}
    private void wakeDisplay(){synchronized(visibilityLock){visibilityLock.notifyAll();}}
    Bitmap snapshot() {
        synchronized(frameLock) {
            if(sharedFrames==null || front<0)return null;
            Bitmap bitmap=Bitmap.createBitmap(geometry.width,geometry.height,Bitmap.Config.ARGB_8888);
            bitmap.copyPixelsFromBuffer(sharedFrames.slot(front));return bitmap;
        }
    }
    private final int refreshHz;
    DirectSession(Context context,DisplayGeometry geometry,int refreshHz,Listener listener) {
        this.context=context;this.geometry=geometry;this.refreshHz=refreshHz;this.listener=listener;worker=new Thread(this::run,"PocketAndroid9");
    }
    void start(){worker.start();}
    boolean running(){return init!=null && init.isAlive() && !closed;}
    void visible(boolean value){visible=value;wakeDisplay();}
    private void status(String text){main.post(()->{if(!closed)listener.status(text);});}
    private void run() {
        String ended="Android 9 stopped. Tap Start to boot again.";
        try {
            // A previous app process may have been killed while Android 9 was up.
            int leftover=DirectImage.stopLeftovers(DirectImage.root(context));
            if(leftover>0) {
                android.util.Log.w("PocketRuntime","Stopped "+leftover+" Android 9 process(es) left by an earlier app process");
                status("Stopped Android 9 left running by an earlier app process.");
            }
            final int[] percent={-1};
            root=DirectImage.prepare(context,geometry,refreshHz,(done,total)->{
                int value=total==0?0:(int)(100*done/total);
                if(value!=percent[0]){percent[0]=value;status("Setting up Android 9: "+value+"%");}
            });
            synchronized(this) {
                if(closed)return;
                java.nio.file.Files.deleteIfExists(new File(root,"logs/display-performance.json").toPath());
                battery=new BootBattery(context,root);hostBroker=new HostBroker(context,root);init=DirectImage.start(root);
                runtimeLog=new RuntimeLog(new File(root,"logs/manager-init.log"));
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
            long presenter=Native.createFramePresenter();
            try(Connection control=frames;ComposerConnection channel=new ComposerConnection(shared)) {
                synchronized(frameLock){sharedFrames=shared;front=-1;}
                ready=true;main.post(()->{if(!closed)listener.ready();});
                long sequence=0,renderedGeneration=-1;FrameReport report=new FrameReport(presenter,channel.fps,shared.frameBytes);
                int release=-1;boolean active=false;
                while(!closed && init.isAlive()) {
                    synchronized(visibilityLock) {
                        if(!visible || surface==null || !surface.isValid()) {
                            if(active){channel.command(3);active=false;}
                            while(!closed && init.isAlive() && (!visible || surface==null || !surface.isValid()))visibilityLock.wait(1000);
                        }
                    }
                    if(closed || !init.isAlive())break;
                    if(!active){channel.command(1);active=true;report.resume();}
                    synchronized(frameLock) {
                        Surface target=surface;
                        if(front>=0 && surfaceGeneration!=renderedGeneration && target!=null && target.isValid()) {
                            long began=SystemClock.elapsedRealtimeNanos();
                            boolean shown=Native.presentFrame(presenter,target,shared.slot(front),geometry.width,geometry.height);
                            if(shown)renderedGeneration=surfaceGeneration;
                            report.redraw(SystemClock.elapsedRealtimeNanos()-began,shown);
                        }
                    }
                    channel.out.writeInt(2);channel.out.writeLong(sequence);channel.out.writeInt(release);channel.out.flush();release=-1;
                    long actual=channel.in.readLong();
                    if(actual==0){report.sample();continue;} // Idle output has no screenshot or pixel copy.
                    int slot=channel.in.readInt();long capture=channel.in.readLong();
                    if(actual<=sequence || slot<0 || slot>=dev.kreijstal.aosppocket.bridge.SharedFrames.SLOT_COUNT || capture<0)
                        throw new IOException("Invalid composer buffer ownership");
                    sequence=actual;
                    long presentBegan=SystemClock.elapsedRealtimeNanos();boolean presented=false;
                    synchronized(frameLock) {
                        release=front;front=slot;
                        Surface target=surface;
                        if(!closed && visible && target!=null && target.isValid())
                            presented=Native.presentFrame(presenter,target,shared.slot(slot),geometry.width,geometry.height);
                        renderedGeneration=presented?surfaceGeneration:-1;
                    }
                    report.record(sequence,capture,SystemClock.elapsedRealtimeNanos()-presentBegan,presented);
                    report.sample();
                }
            } finally {
                synchronized(frameLock){sharedFrames=null;front=-1;shared.close();}
                Native.destroyFramePresenter(presenter);
            }

            if(!closed)ended="Android 9 exited with code "+init.exitValue()+". See api28-rootfs/logs/manager-init.log.";
        } catch(InterruptedException cancelled){Thread.currentThread().interrupt();}
        catch(Exception error){if(!closed)ended="Android 9 failed: "+error.getMessage();}
        finally {
            closed=true;ready=false;commands.shutdownNow();operations.shutdownNow();
            for(LocalSocket socket:sockets)try{socket.close();}catch(IOException ignored){}
            java.lang.Process current=init;
            if(current!=null) {
                current.destroy();
                try {if(!current.waitFor(8,TimeUnit.SECONDS)){current.destroyForcibly();current.waitFor(2,TimeUnit.SECONDS);}}
                catch(InterruptedException interrupted){current.destroyForcibly();}
            }
            if(battery!=null)battery.close();
            if(hostBroker!=null)hostBroker.close();
            if(runtimeLog!=null)runtimeLog.close();
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
    private final class FrameReport {
        final long presenter;final int fps,bytes;
        long sequence,received,presented,attempts,intervalFrames,captureTotal,presentTotal,maxCapture;
        long measuredAt=SystemClock.elapsedRealtimeNanos();
        FrameReport(long presenter,int fps,int bytes){this.presenter=presenter;this.fps=fps;this.bytes=bytes;}
        void resume(){measuredAt=SystemClock.elapsedRealtimeNanos();intervalFrames=0;}
        void record(long version,long copy,long duration,boolean shown) {
            sequence=version;received++;captureTotal+=copy;maxCapture=Math.max(maxCapture,copy);redraw(duration,shown);
        }
        void redraw(long duration,boolean shown){attempts++;presentTotal+=duration;if(shown){presented++;intervalFrames++;}}
        void sample()throws Exception {
            long now=SystemClock.elapsedRealtimeNanos(),elapsed=now-measuredAt;
            if(received==0 || elapsed<2000000000L)return;
            String[] gpu=Native.framePresenterInfo(presenter);
            org.json.JSONObject result=new org.json.JSONObject()
                .put("transport","composer-rgba").put("backing","anonymous").put("codec","none")
                .put("pixel_bytes_over_socket",0).put("target_fps",fps).put("screenshot_calls",0)
                .put("presenter",gpu[0]).put("host_gl_renderer",gpu[1]).put("host_gl_vendor",gpu[2]).put("gpu_fallback_reason",gpu[3])
                .put("gpu_buffers",gpu[0].equals("ahardwarebuffer-egl")?2:0)
                .put("width",geometry.width).put("height",geometry.height)
                .put("shared_bytes",(long)dev.kreijstal.aosppocket.bridge.SharedFrames.SLOT_COUNT*bytes)
                .put("frames_presented",presented).put("frames_received",received).put("composer_sequence",sequence)
                .put("interval_fps",intervalFrames*1000000000.0/elapsed)
                .put("mean_capture_ms",captureTotal/received/1000000.0).put("mean_copy_ms",captureTotal/received/1000000.0)
                .put("max_capture_ms",maxCapture/1000000.0).put("mean_present_ms",presentTotal/Math.max(1,attempts)/1000000.0);
            java.nio.file.Files.write(new File(root,"logs/display-performance.json").toPath(),result.toString().getBytes("UTF-8"));
            android.util.Log.i("PocketFrames",result.toString());measuredAt=now;intervalFrames=0;
        }
    }
    private final class ComposerConnection implements AutoCloseable {
        final LocalSocket socket=new LocalSocket();
        final DataInputStream in;
        final DataOutputStream out;
        final int fps;
        ComposerConnection(AnonymousFrames frames)throws IOException {
            sockets.add(socket);
            try {
                if(closed)throw new IOException("Session closed");
                socket.connect(new LocalSocketAddress(new File(root,"dev/socket/pocket_composer").getPath(),LocalSocketAddress.Namespace.FILESYSTEM));
                socket.setSoTimeout(15000);
                in=new DataInputStream(new BufferedInputStream(socket.getInputStream()));
                out=new DataOutputStream(new BufferedOutputStream(socket.getOutputStream()));
                if(in.readInt()!=0x504b4331 || in.readInt()!=geometry.width || in.readInt()!=geometry.height)
                    throw new IOException("Composer geometry does not match the guest");
                fps=in.readInt();if(fps<1 || fps>120)throw new IOException("Invalid composer refresh rate");
                socket.setFileDescriptorsForSend(new FileDescriptor[]{frames.descriptor()});
                socket.getOutputStream().write(0);socket.setFileDescriptorsForSend(null);
                if(in.readInt()!=1)throw new IOException("Composer refused shared memory");
            }catch(IOException error){close();throw error;}
        }
        void command(int op)throws IOException {out.writeInt(op);out.flush();if(in.readInt()!=1)throw new IOException("Composer control failed");}
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
    private void submitOperation(Command command) {
        if(!ready || closed)return;
        try {operations.execute(()->{
            // GuestControl serves each connection on its own thread. A second
            // socket is essential: sharing input's stream would still block it.
            try(Connection connection=new Connection()){command.run(connection);}
            catch(Exception error){
                String detail=error.getMessage();
                if(detail==null || detail.isEmpty())detail=error instanceof EOFException
                    ? "Guest closed the connection without returning a result."
                    : error.getClass().getSimpleName();
                String message="Android 9 operation failed: "+detail;
                android.util.Log.e("PocketOperations",message,error);
                status(message);
                main.post(()->android.widget.Toast.makeText(context,message,android.widget.Toast.LENGTH_LONG).show());
            }
        });}catch(RejectedExecutionException full){status("Guest operation queue is busy; try again.");}
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
    void importHostContacts(){submitOperation(c->{c.socket.setSoTimeout(90000);c.out.writeInt(5);c.out.flush();status(c.in.readUTF());});}
    void checkHostBridges(){submitOperation(c->{
        c.socket.setSoTimeout(30000);
        try {
            c.out.writeInt(6);c.out.writeUTF("https://example.com/");c.out.flush();String result=c.in.readUTF();
            java.nio.file.Files.write(new File(root,"logs/host-bridge-check.json").toPath(),result.getBytes("UTF-8"));
            org.json.JSONObject report=new org.json.JSONObject(result);
            status("Guest network: "+(report.optBoolean("guest_network_connected")?"connected":"offline")+
                " · HTTPS "+(report.has("http_status")?report.optInt("http_status"):report.optString("http_error","unavailable")));
        } finally {c.socket.setSoTimeout(15000);}
    });}
    void checkHostDevices(){submitOperation(c->{
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
    void openGuestDialer(){submitOperation(c->{c.out.writeInt(8);c.out.flush();if(!c.in.readBoolean())throw new IOException("Guest dialer did not open");});}
    boolean hasLastApp(){return !context.getSharedPreferences("guest-last-app",Context.MODE_PRIVATE).getString("package","").isEmpty();}
    void openLastApp(){submitOperation(c->{
        String name=context.getSharedPreferences("guest-last-app",Context.MODE_PRIVATE).getString("package","");
        if(name.isEmpty())throw new IOException("Install a guest APK first");
        c.out.writeInt(9);c.out.writeUTF(name);c.out.flush();
        if(!c.in.readBoolean())throw new IOException("App did not open");
    });}
    /** Installs one package: {@code base.apk} plus any split APKs, keyed by their file name. */
    void install(java.util.Map<String,android.net.Uri> apks) {
        submitOperation(c->{
            File staged=new File(root,"system-data/pocket-import");
            deleteTree(staged);staged.mkdirs();
            try {
                int index=0;
                for(java.util.Map.Entry<String,android.net.Uri> apk:apks.entrySet()) {
                    status("Copying "+apk.getKey()+" ("+(++index)+"/"+apks.size()+")…");
                    File target=new File(staged,apk.getKey());
                    try(InputStream in=context.getContentResolver().openInputStream(apk.getValue());OutputStream out=new FileOutputStream(target)) {
                        if(in==null)throw new IOException("Cannot open selected APK");
                        GuestImage.copy(in,out,2L*1024*1024*1024);
                    }
                }
                status("Installing APKs in Android 9…");
                c.socket.setSoTimeout(90000);c.out.writeInt(4);c.out.flush();
                String result=c.in.readUTF();
                if(!result.startsWith("Installed "))throw new IOException(result);
                if(result.startsWith("Installed "))context.getSharedPreferences("guest-last-app",Context.MODE_PRIVATE)
                    .edit().putString("package",result.substring(10)).apply();
                status(result);
            } finally {deleteTree(staged);c.socket.setSoTimeout(15000);}
        });
    }
    private static void deleteTree(File file) {
        File[] children=file.listFiles();
        if(children!=null)for(File child:children)deleteTree(child);
        file.delete();
    }
    @Override public synchronized void close() {
        closed=true;ready=false;wakeDisplay();commands.shutdownNow();operations.shutdownNow();
        for(LocalSocket socket:sockets)try{socket.close();}catch(IOException ignored){}
        if(init!=null)init.destroy();
        // Do not interrupt waitFor during supervised cleanup: init needs time
        // to stop and reap its services before a new session can start.
        if(init==null)worker.interrupt();
    }
}
