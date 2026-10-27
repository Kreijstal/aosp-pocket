// SPDX-License-Identifier: GPL-3.0-only
import android.graphics.PixelFormat;
import android.hardware.display.IVirtualDisplayCallback;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.media.MediaMuxer;
import android.media.projection.IMediaProjectionCallback;
import android.os.HandlerThread;
import android.os.Handler;
import android.os.IBinder;
import android.os.SystemClock;
import android.view.Surface;
import java.io.File;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;

/**
 * Screen capture through the guest's MediaProjection service, as a recorder app would after consent:
 * a projection grant, a virtual display mirroring the screen, pixel content through an ImageReader,
 * then a few seconds of H.264 recorded to MP4 while the notification shade animates. Runs as the system
 * uid; the consent dialog is not used.
 */
public final class ProjectionProbe {
    static final int WIDTH=720,HEIGHT=1280,DPI=320,AUTO_MIRROR=16; // DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR
    static Object displays,projection;

    static Method method(Object target,String name) {
        for(Method m:target.getClass().getMethods())if(m.getName().equals(name))return m;
        throw new AssertionError("Missing method "+name);
    }
    static Object service(String name,String stub) throws Exception {
        IBinder binder=(IBinder)Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,name);
        if(binder==null)throw new AssertionError("Guest service missing: "+name);
        return Class.forName(stub+"$Stub").getMethod("asInterface",IBinder.class).invoke(null,binder);
    }
    static IVirtualDisplayCallback.Stub display(Surface surface,String name) throws Exception {
        IVirtualDisplayCallback.Stub callback=new IVirtualDisplayCallback.Stub() {
            public void onPaused(){} public void onResumed(){} public void onStopped(){}
        };
        int id=(Integer)method(displays,"createVirtualDisplay").invoke(displays,callback,projection,"android",name,WIDTH,HEIGHT,DPI,surface,AUTO_MIRROR,null);
        System.out.println("PROJECTION_PROBE virtual_display="+name+" id="+id);
        return callback;
    }

    public static void main(String[] args) throws Exception {
        int seconds=args.length>0?Integer.parseInt(args[0]):3;
        // The display service checks that the caller owns the package it names; root owns none.
        Class.forName("android.os.Process").getMethod("setUid",int.class).invoke(null,1000);
        Object projections=service("media_projection","android.media.projection.IMediaProjectionManager");
        projection=method(projections,"createProjection").invoke(projections,1000,"android",0,false);
        method(projection,"start").invoke(projection,new IMediaProjectionCallback.Stub(){public void onStop(){}});
        System.out.println("PROJECTION_PROBE projection_valid="+method(projections,"isValidMediaProjection").invoke(projections,projection));
        // A sleeping screen mirrors as black: wake it as a power key press would.
        Object power=service("power","android.os.IPowerManager");
        method(power,"wakeUp").invoke(power,SystemClock.uptimeMillis(),"ProjectionProbe","android");
        Thread.sleep(1000);
        displays=service("display","android.hardware.display.IDisplayManager");
        HandlerThread thread=new HandlerThread("ProjectionProbe");thread.start();

        // 1. Mirrored pixels: the screen must not be uniform.
        ImageReader reader=ImageReader.newInstance(WIDTH,HEIGHT,PixelFormat.RGBA_8888,2);
        IVirtualDisplayCallback.Stub first=display(reader.getSurface(),"pocket-projection-pixels");
        Image image=null;
        for(long deadline=SystemClock.elapsedRealtime()+5000;image==null && SystemClock.elapsedRealtime()<deadline;Thread.sleep(50))
            image=reader.acquireLatestImage();
        method(displays,"releaseVirtualDisplay").invoke(displays,first);
        double mean=0,deviation=0;
        if(image!=null) {
            Image.Plane plane=image.getPlanes()[0];ByteBuffer pixels=plane.getBuffer();
            double sum=0,squares=0;long count=0;
            for(int y=0;y<HEIGHT;y+=16)for(int x=0;x<WIDTH;x+=16) {
                int base=y*plane.getRowStride()+x*plane.getPixelStride();
                double luma=0.299*(pixels.get(base)&255)+0.587*(pixels.get(base+1)&255)+0.114*(pixels.get(base+2)&255);
                sum+=luma;squares+=luma*luma;count++;
            }
            mean=sum/count;deviation=Math.sqrt(Math.max(0,squares/count-mean*mean));
            // Keep the mirrored frame (tightly packed RGBA) to compare its colors with the recording.
            byte[] row=new byte[WIDTH*4];
            try(java.io.FileOutputStream out=new java.io.FileOutputStream("/data/local/tmp/pocket-projection.rgba")) {
                for(int y=0;y<HEIGHT;y++){pixels.position(y*plane.getRowStride());pixels.get(row);out.write(row);}
            }
            image.close();
        }
        reader.close();
        System.out.println("PROJECTION_PROBE pixels frame="+(image!=null)+" luma_mean="+Math.round(mean)+" luma_deviation="+Math.round(deviation));

        // 2. Recording: the stock H.264 encoder fed by a mirrored display, muxed to MP4.
        MediaFormat format=MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC,WIDTH,HEIGHT);
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT,MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        format.setInteger(MediaFormat.KEY_BIT_RATE,2000000);format.setInteger(MediaFormat.KEY_FRAME_RATE,30);
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL,1);
        MediaCodec encoder=MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
        System.out.println("PROJECTION_PROBE encoder="+encoder.getName());
        encoder.configure(format,null,null,MediaCodec.CONFIGURE_FLAG_ENCODE);
        Surface input=encoder.createInputSurface();encoder.start();
        File file=new File("/data/local/tmp/pocket-projection.mp4");file.delete();
        MediaMuxer muxer=new MediaMuxer(file.getPath(),MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
        IVirtualDisplayCallback.Stub second=display(input,"pocket-projection-recording");
        // A mirror only produces frames when the screen changes: animate the notification shade meanwhile.
        Object statusBar=service("statusbar","com.android.internal.statusbar.IStatusBarService");
        Thread animator=new Thread(()->{
            try {
                for(boolean open=true;!Thread.currentThread().isInterrupted();open=!open) {
                    method(statusBar,open?"expandNotificationsPanel":"collapsePanels").invoke(statusBar);
                    Thread.sleep(700);
                }
            } catch(InterruptedException stopped) {
            } catch(Exception error) {System.out.println("PROJECTION_PROBE animation_failed="+error);}
        });
        animator.start();
        MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();
        int track=-1,frames=0,keyFrames=0;long bytes=0,start=SystemClock.elapsedRealtime();boolean ending=false;
        while(true) {
            if(!ending && SystemClock.elapsedRealtime()-start>=seconds*1000L) {
                // As AOSP screenrecord: end the input and drain the encoder before the display goes away.
                animator.interrupt();animator.join();
                method(statusBar,"collapsePanels").invoke(statusBar);
                encoder.signalEndOfInputStream();ending=true;
            }
            int index=encoder.dequeueOutputBuffer(info,100000);
            if(index==MediaCodec.INFO_OUTPUT_FORMAT_CHANGED){track=muxer.addTrack(encoder.getOutputFormat());muxer.start();continue;}
            if(index<0){if(ending && SystemClock.elapsedRealtime()-start>seconds*1000L+5000)break;continue;}
            if(info.size>0 && track>=0 && (info.flags&MediaCodec.BUFFER_FLAG_CODEC_CONFIG)==0) {
                muxer.writeSampleData(track,encoder.getOutputBuffer(index),info);
                frames++;bytes+=info.size;if((info.flags&MediaCodec.BUFFER_FLAG_KEY_FRAME)!=0)keyFrames++;
            }
            encoder.releaseOutputBuffer(index,false);
            if((info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0)break;
        }
        method(displays,"releaseVirtualDisplay").invoke(displays,second);
        encoder.stop();encoder.release();
        if(track>=0)muxer.stop();
        muxer.release();
        method(projection,"stop").invoke(projection);
        System.out.println("PROJECTION_PROBE recording frames="+frames+" key_frames="+keyFrames+" bytes="+bytes+" file_bytes="+file.length());
        // A mirrored, non-uniform screen and a recording with frames are both required.
        boolean ok=image!=null && deviation>2 && frames>=10 && keyFrames>=1 && file.length()>1000;
        System.out.println(ok?"PROJECTION_PROBE_PASS":"PROJECTION_PROBE_FAIL");
        thread.quitSafely();
        System.exit(ok?0:1);
    }
}
