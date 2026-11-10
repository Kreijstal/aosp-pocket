// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.ParcelFileDescriptor;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.io.*;
import java.util.concurrent.*;
import org.json.JSONObject;

/** Ordered host input for the API 32 guest's owner-authenticated InputManager bridge. */
public final class GuestInput32 implements AutoCloseable {
    private final File root;
    private final int width,height;
    private final ThreadPoolExecutor queue=new ThreadPoolExecutor(1,1,0,TimeUnit.SECONDS,new ArrayBlockingQueue<>(64));
    private volatile boolean closed;
    private volatile LocalSocket socket;
    private DataInputStream in;
    private DataOutputStream out;
    private volatile long touches,keys,rejected;
    private volatile String error;

    public GuestInput32(File root,int width,int height) {
        if(width<1 || height<1 || width>8192 || height>8192)throw new IllegalArgumentException("Invalid input geometry");
        this.root=root;this.width=width;this.height=height;
    }
    private void connect()throws IOException {
        if(socket!=null)return;
        LocalSocket peer=new LocalSocket();socket=peer;
        if(closed){peer.close();throw new IOException("Input closed");}
        peer.connect(new LocalSocketAddress(new File(root,"dev/socket/pocket_control32").getPath(),LocalSocketAddress.Namespace.FILESYSTEM));
        peer.setSoTimeout(3000);in=new DataInputStream(peer.getInputStream());out=new DataOutputStream(peer.getOutputStream());
        if(in.readInt()!=0x504b4932 || in.readInt()!=width || in.readInt()!=height)throw new IOException("Input geometry mismatch");
        try(ParcelFileDescriptor lifetime=ParcelFileDescriptor.open(new File("/proc/self/stat"),ParcelFileDescriptor.MODE_READ_ONLY)) {
            peer.setFileDescriptorsForSend(new FileDescriptor[]{lifetime.getFileDescriptor()});
            peer.getOutputStream().write(0);peer.setFileDescriptorsForSend(null);
            if(!in.readBoolean())throw new IOException("Host lifetime rejected");
        }
    }
    private interface Packet {void write(DataOutputStream output)throws IOException;}
    /** Connect before publishing a newly booted session to the UI. */
    public void prepare()throws IOException {connect();}
    private void submit(Packet packet,boolean key,boolean motion) {
        if(closed)return;
        try{InputCommandQueue.execute(queue,()->{
            if(closed)return;
            try {
                connect();packet.write(out);out.flush();
                if(in.readBoolean()){if(key)keys++;else touches++;}else rejected++;
            }catch(Exception failure) {
                rejected++;error=failure.toString();LocalSocket peer=socket;socket=null;
                if(peer!=null)try{peer.close();}catch(IOException ignored){}
            }
        },motion);}catch(RejectedExecutionException full){rejected++;error="Guest input queue is full";}
    }
    public void touch(MotionEvent event,float scaleX,float scaleY) {
        int count=event.getPointerCount();if(count<1 || count>10 || !Float.isFinite(scaleX) || !Float.isFinite(scaleY))return;
        final int action=event.getAction();final long down=event.getDownTime(),time=event.getEventTime();
        int[] ids=new int[count];float[] x=new float[count],y=new float[count];
        for(int n=0;n<count;n++){ids[n]=event.getPointerId(n);x[n]=event.getX(n)*scaleX;y[n]=event.getY(n)*scaleY;}
        // Apps never receive the phone's power key, and Android drops touches while
        // the guest screen is off (the lock screen sleeps after 10 s). Precede each
        // touch-down with KEYCODE_WAKEUP: it wakes a sleeping guest and is ignored by
        // an awake one, so a tap on Pocket's display always brings the guest back.
        if(event.getActionMasked()==MotionEvent.ACTION_DOWN)wake(down);
        submit(output->{
            output.writeInt(2);output.writeLong(down);output.writeLong(time);output.writeInt(action);output.writeInt(count);
            for(int n=0;n<count;n++){output.writeInt(ids[n]);output.writeFloat(x[n]);output.writeFloat(y[n]);}
        },false,event.getActionMasked()==MotionEvent.ACTION_MOVE);
    }
    private void wake(long time) {
        for(int action:new int[]{KeyEvent.ACTION_DOWN,KeyEvent.ACTION_UP})submit(output->{
            output.writeInt(3);output.writeLong(time);output.writeLong(time);output.writeInt(action);
            output.writeInt(KeyEvent.KEYCODE_WAKEUP);output.writeInt(0);output.writeInt(0);
        },true,false);
    }
    public boolean key(KeyEvent event) {
        if(closed)return false;
        int action=event.getAction();if(action!=KeyEvent.ACTION_DOWN && action!=KeyEvent.ACTION_UP)return false;
        final long down=event.getDownTime(),time=event.getEventTime();
        final int code=event.getKeyCode(),repeat=event.getRepeatCount(),meta=event.getMetaState();
        // Injected keyboard events do not carry InputReader's wake policy.
        // Match touch-down behavior so typing after guest sleep reaches the
        // focused window instead of being silently consumed while asleep.
        if(action==KeyEvent.ACTION_DOWN && code!=KeyEvent.KEYCODE_WAKEUP
                && code!=KeyEvent.KEYCODE_SLEEP && code!=KeyEvent.KEYCODE_SOFT_SLEEP
                && code!=KeyEvent.KEYCODE_POWER)wake(time);
        submit(output->{
            output.writeInt(3);output.writeLong(down);output.writeLong(time);output.writeInt(action);
            output.writeInt(code);output.writeInt(repeat);output.writeInt(meta);
        },true,false);
        return true;
    }
    public JSONObject report()throws org.json.JSONException {
        return new JSONObject().put("touchEventsAccepted",touches).put("keyEventsAccepted",keys)
            .put("inputEventsAccepted",touches+keys).put("inputEventsRejected",rejected).put("inputError",error);
    }
    @Override public void close() {
        closed=true;queue.shutdownNow();LocalSocket peer=socket;
        if(peer!=null)try{peer.close();}catch(IOException ignored){}
    }
}
