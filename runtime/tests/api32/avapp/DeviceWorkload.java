// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketav32test;

import android.content.Context;
import android.graphics.*;
import android.hardware.camera2.*;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.*;
import android.os.*;
import android.util.Size;
import java.nio.ByteBuffer;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import org.json.*;

final class DeviceWorkload {
    private static void require(boolean value,String why)throws Exception {if(!value)throw new java.io.IOException(why);}
    static JSONObject playback()throws Exception {
        int rate=48000,frames=rate*2,minimum=AudioTrack.getMinBufferSize(rate,AudioFormat.CHANNEL_OUT_STEREO,AudioFormat.ENCODING_PCM_16BIT);
        require(minimum>0,"AudioTrack minimum buffer unavailable");
        short[] pcm=new short[frames*2];for(int i=0;i<frames;i++)pcm[i*2]=pcm[i*2+1]=(short)(2000*Math.sin(i*2*Math.PI*440/rate));
        AudioTrack track=new AudioTrack.Builder().setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
            .setAudioFormat(new AudioFormat.Builder().setSampleRate(rate).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
            .setTransferMode(AudioTrack.MODE_STREAM).setBufferSizeInBytes(Math.max(minimum,rate*4/5)).build();
        try {
            require(track.getState()==AudioTrack.STATE_INITIALIZED,"AudioTrack initialization failed");
            long begin=SystemClock.elapsedRealtime(),deadline=begin+15000;int written=0;track.play();
            while(written<pcm.length) {
                int count=track.write(pcm,written,Math.min(pcm.length-written,4800),AudioTrack.WRITE_NON_BLOCKING);
                require(count>=0,"AudioTrack write error "+count);written+=count;
                require(SystemClock.elapsedRealtime()<deadline,"AudioTrack write timed out");if(count==0)Thread.sleep(5);
            }
            long played;
            while((played=Integer.toUnsignedLong(track.getPlaybackHeadPosition()))<frames) {
                require(SystemClock.elapsedRealtime()<deadline,"AudioTrack playback head did not advance");Thread.sleep(10);
            }
            return new JSONObject().put("sampleRate",rate).put("channels",2).put("writtenFrames",written/2).put("playedFrames",played)
                .put("elapsedMillis",SystemClock.elapsedRealtime()-begin).put("underruns",track.getUnderrunCount());
        }finally{try{track.stop();}catch(IllegalStateException ignored){}track.release();}
    }
    static JSONObject microphone()throws Exception {
        int rate=48000,target=rate*2,minimum=AudioRecord.getMinBufferSize(rate,AudioFormat.CHANNEL_IN_MONO,AudioFormat.ENCODING_PCM_16BIT);
        require(minimum>0,"AudioRecord minimum buffer unavailable");
        AudioRecord record=new AudioRecord.Builder().setAudioSource(MediaRecorder.AudioSource.MIC)
            .setAudioFormat(new AudioFormat.Builder().setSampleRate(rate).setChannelMask(AudioFormat.CHANNEL_IN_MONO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
            .setBufferSizeInBytes(Math.max(minimum,rate*2/5)).build();
        try {
            require(record.getState()==AudioRecord.STATE_INITIALIZED,"AudioRecord initialization failed");
            long begin=SystemClock.elapsedRealtime(),deadline=begin+15000;record.startRecording();
            require(record.getRecordingState()==AudioRecord.RECORDSTATE_RECORDING,"AudioRecord did not start");
            short[] buffer=new short[4800];int samples=0,reads=0;double squares=0;
            while(samples<target) {
                int count=record.read(buffer,0,Math.min(buffer.length,target-samples),AudioRecord.READ_NON_BLOCKING);
                require(count>=0,"AudioRecord read error "+count);samples+=count;if(count>0)reads++;
                for(int i=0;i<count;i++)squares+=(double)buffer[i]*buffer[i];
                require(SystemClock.elapsedRealtime()<deadline,"AudioRecord read timed out");if(count==0)Thread.sleep(5);
            }
            AudioTimestamp timestamp=new AudioTimestamp();int status=record.getTimestamp(timestamp,AudioTimestamp.TIMEBASE_MONOTONIC);
            require(status==AudioRecord.SUCCESS && timestamp.framePosition>0 && timestamp.nanoTime>0,"AudioRecord timestamp unavailable: "+status);
            return new JSONObject().put("sampleRate",rate).put("channels",1).put("samples",samples).put("reads",reads)
                .put("rms",Math.sqrt(squares/samples)).put("timestampFrame",timestamp.framePosition).put("timestampNanos",timestamp.nanoTime)
                .put("elapsedMillis",SystemClock.elapsedRealtime()-begin);
        }finally{try{record.stop();}catch(IllegalStateException ignored){}record.release();}
    }
    private static Size choose(Size[] sizes)throws Exception {
        require(sizes!=null && sizes.length>0,"Camera output sizes missing");
        return Arrays.stream(sizes).filter(s->s.getWidth()>=160 && s.getHeight()>=120)
            .min(Comparator.comparingLong(s->(long)s.getWidth()*s.getHeight())).orElseThrow(()->new IllegalStateException("No usable camera size"));
    }
    static JSONObject camera(Context context)throws Exception {
        CameraManager manager=context.getSystemService(CameraManager.class);String[] ids=manager.getCameraIdList();require(ids.length>0,"No guest cameras advertised");
        return camera(context,ids[0]);
    }
    static JSONObject camera(Context context,String id)throws Exception {
        CameraManager manager=context.getSystemService(CameraManager.class);String[] ids=manager.getCameraIdList();
        require(Arrays.asList(ids).contains(id),"Requested camera is not advertised");
        CameraCharacteristics info=manager.getCameraCharacteristics(id);
        StreamConfigurationMap map=info.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);require(map!=null,"Camera stream map missing");
        Size preview=choose(map.getOutputSizes(ImageFormat.YUV_420_888)),still=choose(map.getOutputSizes(ImageFormat.JPEG));
        HandlerThread thread=new HandlerThread("PublicCamera32");thread.start();Handler handler=new Handler(thread.getLooper());
        AtomicReference<CameraDevice> device=new AtomicReference<>();AtomicReference<CameraCaptureSession> session=new AtomicReference<>();
        AtomicReference<Throwable> failure=new AtomicReference<>();AtomicBoolean cancelled=new AtomicBoolean();
        CountDownLatch opened=new CountDownLatch(1),configured=new CountDownLatch(1),previewReady=new CountDownLatch(1),jpegReady=new CountDownLatch(1),closed=new CountDownLatch(1);
        JSONArray frames=new JSONArray();AtomicReference<JSONObject> jpeg=new AtomicReference<>();AtomicInteger completions=new AtomicInteger();
        Runnable failed=()->{opened.countDown();configured.countDown();previewReady.countDown();jpegReady.countDown();};
        ImageReader yuv=ImageReader.newInstance(preview.getWidth(),preview.getHeight(),ImageFormat.YUV_420_888,3);
        ImageReader jpg=ImageReader.newInstance(still.getWidth(),still.getHeight(),ImageFormat.JPEG,2);
        try {
            yuv.setOnImageAvailableListener(reader->{
                try(Image image=reader.acquireLatestImage()) {
                    if(image==null || cancelled.get() || frames.length()>=3)return;
                    require(image.getFormat()==ImageFormat.YUV_420_888 && image.getWidth()==preview.getWidth() && image.getHeight()==preview.getHeight(),"Unexpected preview image");
                    Image.Plane plane=image.getPlanes()[0];ByteBuffer data=plane.getBuffer();int min=255,max=0;long sum=0;
                    for(int row=0;row<image.getHeight();row+=4)for(int col=0;col<image.getWidth();col+=4) {
                        int v=data.get(row*plane.getRowStride()+col*plane.getPixelStride())&255;min=Math.min(min,v);max=Math.max(max,v);sum+=v;
                    }
                    require(image.getTimestamp()>0 && max>min,"Preview image has no timestamp or luma variation");
                    if(frames.length()>0)require(image.getTimestamp()>frames.getJSONObject(frames.length()-1).getLong("timestampNanos"),"Preview timestamps did not advance");
                    frames.put(new JSONObject().put("timestampNanos",image.getTimestamp()).put("minLuma",min).put("maxLuma",max).put("sampleSum",sum));
                    if(frames.length()==3)previewReady.countDown();
                }catch(Throwable error){failure.compareAndSet(null,error);failed.run();}
            },handler);
            jpg.setOnImageAvailableListener(reader->{
                try(Image image=reader.acquireLatestImage()) {
                    if(image==null || cancelled.get())return;
                    ByteBuffer data=image.getPlanes()[0].getBuffer();byte[] bytes=new byte[data.remaining()];data.get(bytes);
                    require(image.getFormat()==ImageFormat.JPEG && bytes.length>100 && (bytes[0]&255)==255 && (bytes[1]&255)==216,"Invalid JPEG output");
                    Bitmap bitmap=BitmapFactory.decodeByteArray(bytes,0,bytes.length);require(bitmap!=null,"JPEG decode failed");
                    try {
                        require(bitmap.getWidth()==still.getWidth() && bitmap.getHeight()==still.getHeight(),"JPEG dimensions differ from configuration");
                        Set<Integer> colors=new HashSet<>();for(int y=0;y<bitmap.getHeight();y+=8)for(int x=0;x<bitmap.getWidth();x+=8)colors.add(bitmap.getPixel(x,y));
                        require(colors.size()>1,"JPEG has no color variation");
                        byte[] hash=java.security.MessageDigest.getInstance("SHA-256").digest(bytes);StringBuilder hex=new StringBuilder();for(byte b:hash)hex.append(String.format("%02x",b&255));
                        jpeg.set(new JSONObject().put("bytes",bytes.length).put("width",bitmap.getWidth()).put("height",bitmap.getHeight()).put("sampledColors",colors.size()).put("timestampNanos",image.getTimestamp()).put("sha256",hex));
                    }finally{bitmap.recycle();}
                    jpegReady.countDown();
                }catch(Throwable error){failure.compareAndSet(null,error);failed.run();}
            },handler);
            manager.openCamera(id,new CameraDevice.StateCallback() {
                public void onOpened(CameraDevice value){device.set(value);if(cancelled.get())value.close();opened.countDown();}
                public void onDisconnected(CameraDevice value){value.close();failure.compareAndSet(null,new java.io.IOException("Camera disconnected"));failed.run();}
                public void onError(CameraDevice value,int error){value.close();failure.compareAndSet(null,new java.io.IOException("Camera device error "+error));failed.run();}
                public void onClosed(CameraDevice value){closed.countDown();}
            },handler);
            require(opened.await(15,TimeUnit.SECONDS),"Camera open timed out");if(failure.get()!=null)throw new java.io.IOException("Camera open failed",failure.get());require(device.get()!=null,"No opened camera");
            device.get().createCaptureSession(Arrays.asList(yuv.getSurface(),jpg.getSurface()),new CameraCaptureSession.StateCallback() {
                public void onConfigured(CameraCaptureSession value){session.set(value);if(cancelled.get())value.close();configured.countDown();}
                public void onConfigureFailed(CameraCaptureSession value){value.close();failure.compareAndSet(null,new java.io.IOException("Camera session configuration failed"));failed.run();}
            },handler);
            require(configured.await(15,TimeUnit.SECONDS),"Camera configuration timed out");if(failure.get()!=null)throw new java.io.IOException("Camera session failed",failure.get());require(session.get()!=null,"No camera session");
            CameraCaptureSession.CaptureCallback callback=new CameraCaptureSession.CaptureCallback() {
                public void onCaptureCompleted(CameraCaptureSession s,CaptureRequest r,TotalCaptureResult result){completions.incrementAndGet();}
                public void onCaptureFailed(CameraCaptureSession s,CaptureRequest r,CaptureFailure error){failure.compareAndSet(null,new java.io.IOException("Camera capture failed "+error.getReason()));failed.run();}
            };
            CaptureRequest.Builder request=device.get().createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);request.addTarget(yuv.getSurface());session.get().setRepeatingRequest(request.build(),callback,handler);
            require(previewReady.await(20,TimeUnit.SECONDS),"Camera preview timed out");if(failure.get()!=null)throw new java.io.IOException("Camera preview failed",failure.get());
            session.get().stopRepeating();CaptureRequest.Builder shot=device.get().createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE);shot.addTarget(jpg.getSurface());
            session.get().capture(shot.build(),callback,handler);require(jpegReady.await(20,TimeUnit.SECONDS),"Camera JPEG timed out");if(failure.get()!=null)throw new java.io.IOException("Camera still failed",failure.get());
            require(frames.length()==3 && jpeg.get()!=null && completions.get()>0,"Camera outputs incomplete");
            return new JSONObject().put("cameraId",id).put("advertisedCameras",new JSONArray(Arrays.asList(ids))).put("previewWidth",preview.getWidth()).put("previewHeight",preview.getHeight())
                .put("previewFrames",frames).put("captureCompletions",completions.get()).put("jpeg",jpeg.get());
        }finally {
            cancelled.set(true);if(session.get()!=null)session.get().close();
            if(device.get()!=null){device.get().close();closed.await(5,TimeUnit.SECONDS);}
            yuv.close();jpg.close();thread.quitSafely();thread.join(5000);require(!thread.isAlive(),"Camera callback thread failed to stop");
        }
    }
}
