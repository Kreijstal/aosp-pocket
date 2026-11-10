// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import android.graphics.Bitmap;
import android.net.*;
import android.os.*;
import android.view.*;
import dev.kreijstal.aosppocket.bridge.*;
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.file.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.JSONObject;

/** Actual compositor frames presented on a host Surface; no guest screenshots. */
public final class HostDisplay32 implements AutoCloseable {
    private final File root,log;
    private final SurfaceView view;
    private final java.lang.Process process;
    private final String runId;
    private final Thread worker;
    private volatile GuestInput32 input;
    private final AtomicBoolean copyPending=new AtomicBoolean();
    private volatile boolean closed;
    private volatile LocalSocket framesSocket;
    private volatile int width,height,refreshHz,guestMatches,hostMatches,pixelCopyStatus=-1;
    private volatile long received,presented,sequence;
    private volatile String error,matchedNonce,matchedTargetToken;
    private volatile String[] guestPatternSamples;
    private volatile String sampleTargetToken;
    private volatile JSONObject ready;

    public HostDisplay32(File root,File log,SurfaceView view,java.lang.Process process,String runId) {
        this.root=root;this.log=log;this.view=view;this.process=process;this.runId=runId;
        view.post(()->{
            view.setFocusable(true);view.setFocusableInTouchMode(true);view.requestFocus();
            view.setOnTouchListener((v,event)->{if(event.getActionMasked()==MotionEvent.ACTION_DOWN)view.requestFocus();touch(event);return true;});
            view.setOnKeyListener((v,code,event)->{GuestInput32 bridge=input;return bridge!=null && bridge.key(event);});
        });
        worker=new Thread(this::run,"Api32HostDisplay");worker.setDaemon(true);worker.start();
    }
    private JSONObject readReady() {
        try {
            File file=new File(root,"tmp/app32-input-ready.json");
            if(!file.isFile() || Files.isSymbolicLink(file.toPath()) || file.length()>8192)return null;
            JSONObject value=new JSONObject(new String(Files.readAllBytes(file.toPath()),"UTF-8"));
            return runId.equals(value.optString("runId"))?value:null;
        }catch(Exception notReady){return null;}
    }
    private static int expected(String nonce,int index) {
        int value=Integer.parseInt(nonce.substring(index*2,index*2+2),16);
        return 0xff000000|(value<<16)|((value^0x5a)<<8)|(37+13*index);
    }
    private int matches(JSONObject target,ByteBuffer pixels,Bitmap bitmap)throws Exception {
        String nonce=target.getString("nonce");if(!nonce.matches("[a-f0-9]{32}"))return 0;
        int left=target.getInt("patternLeft"),top=target.getInt("patternTop"),w=target.getInt("patternWidth"),h=target.getInt("patternHeight");
        if(w<16 || h<1 || left<0 || top<0 || left+w>width || top+h>height)return 0;
        int result=0;String[] samples=pixels==null?null:new String[16];
        for(int n=0;n<16;n++) {
            int x=left+(n*w/16+(n+1)*w/16)/2,y=top+h/2,color;
            if(bitmap!=null)color=bitmap.getPixel(x,y);
            else {int at=(y*width+x)*4;color=(pixels.get(at+3)&255)<<24|(pixels.get(at)&255)<<16|(pixels.get(at+1)&255)<<8|(pixels.get(at+2)&255);}
            if(samples!=null)samples[n]=Integer.toHexString(color);
            if(color==expected(nonce,n))result++;
        }
        if(samples!=null){guestPatternSamples=samples;sampleTargetToken=target.optString("targetToken");}
        return result;
    }
    private void copySurface(JSONObject target) {
        if(!copyPending.compareAndSet(false,true))return;
        view.post(()->{
            if(closed || !view.getHolder().getSurface().isValid()){copyPending.set(false);return;}
            Bitmap bitmap=Bitmap.createBitmap(width,height,Bitmap.Config.ARGB_8888);
            try {
                PixelCopy.request(view,bitmap,status->{
                    pixelCopyStatus=status;
                    try {
                        if(status==PixelCopy.SUCCESS) {
                            int count=matches(target,null,bitmap);hostMatches=count;
                            if(count==16){matchedNonce=target.getString("nonce");matchedTargetToken=target.getString("targetToken");}
                        }
                    }catch(Exception failure){error=failure.toString();}
                    finally{bitmap.recycle();copyPending.set(false);}
                },new Handler(Looper.getMainLooper()));
            }catch(Exception failure){bitmap.recycle();copyPending.set(false);error=failure.toString();}
        });
    }
    private void present(long presenter,ByteBuffer pixels)throws Exception {
        Surface surface=view.getHolder().getSurface();
        if(surface.isValid() && Native.presentFrame(presenter,surface,pixels,width,height)) {
            presented++;JSONObject target=readReady();
            if(target!=null) {
                ready=target;guestMatches=matches(target,pixels,null);
                if(guestMatches==16 && !target.getString("targetToken").equals(matchedTargetToken))copySurface(target);
            }
        }
    }
    private void run() {
        long presenter=0;
        try {
            // The early graphics probe also opens a composer. Wait for the
            // actual SurfaceFlinger service before attaching this consumer.
            while(!closed && process.isAlive()) {
                if(log.isFile() && log.length()<2*1024*1024 && new String(Files.readAllBytes(log.toPath()),"UTF-8").contains("API32_SURFACEFLINGER_READY"))break;
                Thread.sleep(200);
            }
            if(closed || !process.isAlive())return;
            LocalSocket socket=new LocalSocket();framesSocket=socket;
            socket.connect(new LocalSocketAddress(new File(root,"dev/socket/pocket_composer").getPath(),LocalSocketAddress.Namespace.FILESYSTEM));
            socket.setSoTimeout(10000);
            DataInputStream in=new DataInputStream(new BufferedInputStream(socket.getInputStream()));
            DataOutputStream out=new DataOutputStream(new BufferedOutputStream(socket.getOutputStream()));
            if(in.readInt()!=0x504b4331)throw new IOException("Wrong compositor protocol");
            width=in.readInt();height=in.readInt();refreshHz=in.readInt();
            DisplayGeometry geometry=new DisplayGeometry(width,height,160);if(refreshHz<1 || refreshHz>120)throw new IOException("Invalid refresh rate");
            input=new GuestInput32(root,width,height);
            view.post(()->view.getHolder().setFixedSize(width,height));
            try(AnonymousFrames memory=AnonymousFrames.create(geometry)) {
                socket.setFileDescriptorsForSend(new FileDescriptor[]{memory.descriptor()});socket.getOutputStream().write(0);socket.setFileDescriptorsForSend(null);
                if(in.readInt()!=1)throw new IOException("Compositor rejected frame memory");
                out.writeInt(1);out.flush();if(in.readInt()!=1)throw new IOException("Compositor resume failed");
                presenter=Native.createFramePresenter();int release=-1,front=-1;
                while(!closed && process.isAlive()) {
                    out.writeInt(2);out.writeLong(sequence);out.writeInt(release);out.flush();release=-1;
                    long next=in.readLong();if(next==0) {
                        JSONObject target=readReady();
                        if(front>=0 && (presented==0 || (target!=null && !target.getString("targetToken").equals(matchedTargetToken))))present(presenter,memory.slot(front));
                        continue;
                    }
                    int slot=in.readInt();long copied=in.readLong();
                    if(next<=sequence || slot<0 || slot>=SharedFrames.SLOT_COUNT || copied<0)throw new IOException("Invalid frame ownership");
                    sequence=next;received++;release=front;front=slot;present(presenter,memory.slot(slot));
                }
            }
        }catch(Throwable failure){if(!closed)error=failure.toString();}
        finally{if(presenter!=0)Native.destroyFramePresenter(presenter);LocalSocket socket=framesSocket;if(socket!=null)try{socket.close();}catch(Exception ignored){}}
    }
    private void touch(MotionEvent event) {
        GuestInput32 bridge=input;
        if(closed || bridge==null || view.getWidth()<1 || view.getHeight()<1)return;
        bridge.touch(event,(float)width/view.getWidth(),(float)height/view.getHeight());
    }
    public JSONObject report()throws Exception {
        int[] location=new int[2];view.getLocationOnScreen(location);
        JSONObject report=new JSONObject().put("width",width).put("height",height).put("refreshHz",refreshHz).put("framesReceived",received).put("framesPresented",presented)
            .put("guestPatternMatches",guestMatches).put("hostPatternMatches",hostMatches).put("matchedNonce",matchedNonce).put("matchedTargetToken",matchedTargetToken).put("pixelCopyStatus",pixelCopyStatus)
            .put("error",error)
            .put("viewX",location[0]).put("viewY",location[1]).put("viewWidth",view.getWidth()).put("viewHeight",view.getHeight());
        GuestInput32 bridge=input;
        if(bridge!=null){JSONObject state=bridge.report();java.util.Iterator<String> keys=state.keys();while(keys.hasNext()){String key=keys.next();report.put(key,state.get(key));}}
        JSONObject current=readReady();if(current!=null)report.put("inputReady",current);
        File phase=new File(root,"tmp/ui32-phase.json");
        if(phase.isFile() && !Files.isSymbolicLink(phase.toPath()) && phase.length()<8192) {
            try {
                JSONObject ui=new JSONObject(new String(Files.readAllBytes(phase.toPath()),"UTF-8"));
                if(runId.equals(ui.optString("runId")))report.put("uiReady",ui);
            }catch(Exception notReady){}
        }
        String[] samples=guestPatternSamples;
        if(samples!=null)report.put("guestPatternSamples",new org.json.JSONArray(samples)).put("sampleTargetToken",sampleTargetToken);
        return report;
    }
    @Override public void close()throws Exception {
        closed=true;LocalSocket socket=framesSocket;if(socket!=null)socket.close();GuestInput32 bridge=input;if(bridge!=null)bridge.close();
        worker.join(3000);
    }
}
