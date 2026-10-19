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

/** Persistent Android 9 session owned by GuestService, independent of windows. */
final class DirectSession implements AutoCloseable {
    interface Listener {
        void status(String message);
        void ready();
        void frame(Bitmap bitmap);
        void stopped(String message);
    }
    private final Context context;
    private final Listener listener;
    private final Handler main=new Handler(Looper.getMainLooper());
    private final Set<LocalSocket> sockets=ConcurrentHashMap.newKeySet();
    private final ThreadPoolExecutor commands=new ThreadPoolExecutor(1,1,0,TimeUnit.SECONDS,new ArrayBlockingQueue<>(128));
    private final Thread worker;
    private volatile boolean closed,visible,ready;
    private volatile java.lang.Process init;
    private BootBattery battery;
    private File root;
    private Connection input;
    DirectSession(Context context,Listener listener) {
        this.context=context;this.listener=listener;worker=new Thread(this::run,"PocketAndroid9");
    }
    void start(){worker.start();}
    boolean running(){return init!=null && init.isAlive() && !closed;}
    void visible(boolean value){visible=value;}
    private void status(String text){main.post(()->{if(!closed)listener.status(text);});}
    private void run() {
        String ended="Android 9 stopped. Tap Start to boot again.";
        try {
            final int[] percent={-1};
            root=DirectImage.prepare(context,(done,total)->{
                int value=total==0?0:(int)(100*done/total);
                if(value!=percent[0]){percent[0]=value;status("Setting up Android 9: "+value+"%");}
            });
            synchronized(this) {
                if(closed)return;
                battery=new BootBattery(context,root);init=DirectImage.start(root);
            }
            status("Android 9 starting…");
            long deadline=SystemClock.elapsedRealtime()+240000;
            Connection frames=null;
            while(!closed && init.isAlive() && SystemClock.elapsedRealtime()<deadline) {
                try {frames=new Connection();break;}
                catch(IOException notReady){Thread.sleep(300);}
            }
            if(frames==null)throw new IOException("Android 9 did not open its display/input endpoint; see api28-rootfs/logs/manager-init.log");
            try(Connection channel=frames) {
                ready=true;main.post(()->{if(!closed)listener.ready();});
                while(!closed && init.isAlive()) {
                    if(!visible){Thread.sleep(250);continue;}
                    long began=SystemClock.elapsedRealtime();channel.out.writeInt(1);channel.out.flush();
                    int length=channel.in.readInt();if(length<1 || length>8*1024*1024)throw new IOException("Invalid frame length");
                    byte[] pixels=new byte[length];channel.in.readFully(pixels);
                    Bitmap bitmap=BitmapFactory.decodeByteArray(pixels,0,pixels.length);
                    if(bitmap==null || bitmap.getWidth()!=480 || bitmap.getHeight()!=800)throw new IOException("Invalid guest display frame");
                    main.post(()->{if(!closed)listener.frame(bitmap);else bitmap.recycle();});
                    Thread.sleep(Math.max(1,250-(SystemClock.elapsedRealtime()-began)));
                }
            }
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
                if(in.readInt()!=0x504b5439 || in.readInt()!=480 || in.readInt()!=800)throw new IOException("Unsupported guest control protocol");
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
                status(c.in.readUTF());
            } finally {temp.delete();target.delete();c.socket.setSoTimeout(15000);}
        });
    }
    @Override public synchronized void close() {
        closed=true;ready=false;commands.shutdownNow();
        for(LocalSocket socket:sockets)try{socket.close();}catch(IOException ignored){}
        if(init!=null)init.destroy();
        // Do not interrupt waitFor during supervised cleanup: init needs time
        // to stop and reap its services before a new session can start.
        if(init==null)worker.interrupt();
    }
}
