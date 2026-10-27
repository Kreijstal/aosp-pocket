// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.content.Context;
import android.graphics.ImageFormat;
import android.hardware.camera2.*;
import android.media.*;
import android.net.LocalSocket;
import android.os.*;
import android.util.Size;
import org.json.*;
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicBoolean;
import dev.kreijstal.aosppocket.bridge.HostWire;

/** Request-scoped host devices. Raw YUV planes and PCM are never re-encoded. */
final class HostMedia {
    private static final Semaphore cameraUse=new Semaphore(1),microphoneUse=new Semaphore(1),speakerUse=new Semaphore(1),guestOutput=new Semaphore(1);
    private HostMedia() { }
    static JSONObject cameras(Context context) throws Exception {
        CameraManager manager=(CameraManager)context.getSystemService(Context.CAMERA_SERVICE);
        JSONArray devices=new JSONArray();
        for(String id:manager.getCameraIdList()) {
            CameraCharacteristics c=manager.getCameraCharacteristics(id);JSONArray sizes=new JSONArray();
            android.hardware.camera2.params.StreamConfigurationMap map=c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            Size[] supported=map==null?null:map.getOutputSizes(ImageFormat.YUV_420_888);
            if(supported!=null)for(Size s:supported)sizes.put(new JSONArray().put(s.getWidth()).put(s.getHeight()));
            devices.put(new JSONObject().put("id",id).put("facing",c.get(CameraCharacteristics.LENS_FACING))
                .put("sensor_orientation",c.get(CameraCharacteristics.SENSOR_ORIENTATION)).put("yuv420_sizes",sizes));
        }
        return new JSONObject().put("devices",devices).put("source","host_camera2");
    }
    /**
     * Camera list for the guest camera HAL, one item per line: "camera ID FACING ORIENTATION", then that camera's
     * "size WIDTH HEIGHT MIN_FRAME_DURATION_NS" lines for YUV_420_888 and "fps MIN MAX" AE target ranges.
     */
    static byte[] cameraLines(Context context) throws Exception {
        CameraManager manager=(CameraManager)context.getSystemService(Context.CAMERA_SERVICE);
        StringBuilder text=new StringBuilder();
        for(String id:manager.getCameraIdList()) {
            if(!id.matches("[A-Za-z0-9_.-]{1,32}"))continue;
            CameraCharacteristics c=manager.getCameraCharacteristics(id);
            android.hardware.camera2.params.StreamConfigurationMap map=c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            Size[] sizes=map==null?null:map.getOutputSizes(ImageFormat.YUV_420_888);
            if(sizes==null || sizes.length==0)continue;
            Integer facing=c.get(CameraCharacteristics.LENS_FACING),orientation=c.get(CameraCharacteristics.SENSOR_ORIENTATION);
            text.append("camera ").append(id).append(' ').append(facing==null?CameraCharacteristics.LENS_FACING_EXTERNAL:facing)
                .append(' ').append(orientation==null?0:orientation).append('\n');
            for(Size s:sizes)text.append("size ").append(s.getWidth()).append(' ').append(s.getHeight()).append(' ')
                .append(map.getOutputMinFrameDuration(ImageFormat.YUV_420_888,s)).append('\n');
            android.util.Range<Integer>[] ranges=c.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
            if(ranges!=null)for(android.util.Range<Integer> r:ranges)text.append("fps ").append(r.getLower()).append(' ').append(r.getUpper()).append('\n');
        }
        return text.toString().getBytes(StandardCharsets.UTF_8);
    }
    /**
     * Continuous host camera frames for the guest camera HAL. After the reply, each frame is [int32 length]
     * [int64 sensor timestamp ns] and tightly packed I420 (Y, U, V), until the guest closes the socket.
     */
    static void cameraStream(Context context,JSONObject args,DataOutputStream out) throws Exception {
        String id=args.getString("id");int width=args.getInt("width"),height=args.getInt("height"),fps=args.optInt("fps",30);
        CameraManager manager=(CameraManager)context.getSystemService(Context.CAMERA_SERVICE);
        CameraCharacteristics characteristics=manager.getCameraCharacteristics(id);
        android.hardware.camera2.params.StreamConfigurationMap map=characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        Size[] supported=map==null?null:map.getOutputSizes(ImageFormat.YUV_420_888);
        if(supported==null || !Arrays.asList(supported).contains(new Size(width,height)) || width%2!=0 || height%2!=0 || fps<1)
            throw new IllegalArgumentException("Requested camera stream is not supported by the host");
        // The AE range whose upper bound is closest to the requested rate, preferring ones that can reach it.
        android.util.Range<Integer> range=null;
        android.util.Range<Integer>[] ranges=characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
        if(ranges!=null)for(android.util.Range<Integer> r:ranges)
            if(range==null || Math.abs(r.getUpper()-fps)<Math.abs(range.getUpper()-fps) ||
               (r.getUpper().equals(range.getUpper()) && r.getLower()<range.getLower()))range=r;
        // A reconfiguring guest session opens its next stream while the previous one is still closing.
        if(!cameraUse.tryAcquire(3,TimeUnit.SECONDS))throw new IllegalStateException("Host camera is already in use by the guest");
        HandlerThread thread=new HandlerThread("PocketCameraStream");thread.start();Handler handler=new Handler(thread.getLooper());
        AtomicReference<CameraDevice> camera=new AtomicReference<>();AtomicReference<CameraCaptureSession> session=new AtomicReference<>();
        AtomicReference<Exception> failed=new AtomicReference<>();AtomicBoolean cancelled=new AtomicBoolean();
        CountDownLatch configured=new CountDownLatch(1),ended=new CountDownLatch(1);
        java.util.concurrent.atomic.AtomicLong lastFrame=new java.util.concurrent.atomic.AtomicLong();
        Runnable fail=()->{configured.countDown();ended.countDown();};
        try(ImageReader reader=ImageReader.newInstance(width,height,ImageFormat.YUV_420_888,3)) {
            byte[] frame=new byte[width*height*3/2];
            reader.setOnImageAvailableListener(source->{
                try(Image image=source.acquireLatestImage()) {
                    if(image==null || cancelled.get())return;
                    packI420(image,frame,width,height);
                    synchronized(out){out.writeInt(frame.length);out.writeLong(image.getTimestamp());out.write(frame);out.flush();}
                    lastFrame.set(SystemClock.elapsedRealtime());
                } catch(Exception error){failed.compareAndSet(null,error);ended.countDown();}
            },handler);
            manager.openCamera(id,new CameraDevice.StateCallback() {
                public void onOpened(CameraDevice device) {
                    camera.set(device);
                    if(cancelled.get()){device.close();return;}
                    try {device.createCaptureSession(Collections.singletonList(reader.getSurface()),new CameraCaptureSession.StateCallback() {
                        public void onConfigured(CameraCaptureSession value){session.set(value);configured.countDown();}
                        public void onConfigureFailed(CameraCaptureSession s){s.close();failed.set(new IOException("Host camera session configuration failed"));fail.run();}
                    },handler);}catch(Exception error){failed.set(error);fail.run();}
                }
                public void onDisconnected(CameraDevice device){device.close();failed.set(new IOException("Host camera disconnected"));fail.run();}
                public void onError(CameraDevice device,int code){device.close();failed.set(new IOException("Host camera error "+code));fail.run();}
            },handler);
            if(!configured.await(8,TimeUnit.SECONDS))throw new IOException("Host camera timed out");
            if(failed.get()!=null)throw failed.get();
            CaptureRequest.Builder request=camera.get().createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
            request.addTarget(reader.getSurface());
            if(range!=null)request.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE,range);
            // The reply precedes every frame: the repeating request starts only after it is written.
            synchronized(out) {
                out.writeInt(0);HostWire.write(out,new JSONObject().put("width",width).put("height",height).put("format","i420")
                    .put("fps_range",range==null?JSONObject.NULL:new JSONArray().put(range.getLower()).put(range.getUpper()))
                    .toString().getBytes(StandardCharsets.UTF_8));out.flush();
            }
            lastFrame.set(SystemClock.elapsedRealtime());
            session.get().setRepeatingRequest(request.build(),null,handler);
            // Ends when the guest closes the stream (the next write fails) or the host camera stops delivering.
            while(!ended.await(200,TimeUnit.MILLISECONDS))
                if(SystemClock.elapsedRealtime()-lastFrame.get()>5000)throw new IOException("Host camera stopped delivering frames");
            if(failed.get()!=null)throw failed.get();
        } finally {
            cancelled.set(true);
            try{if(session.get()!=null)session.get().close();}catch(Exception ignored){}
            if(camera.get()!=null)camera.get().close();
            thread.quitSafely();try{thread.join(1000);}finally{cameraUse.release();}
        }
    }
    private static void packI420(Image image,byte[] out,int width,int height) {
        Image.Plane[] planes=image.getPlanes();int offset=0;
        ByteBuffer y=planes[0].getBuffer();int stride=planes[0].getRowStride();
        for(int row=0;row<height;row++){y.position(row*stride);y.get(out,offset,width);offset+=width;}
        int cw=width/2,ch=height/2;
        for(int p=1;p<3;p++) {
            ByteBuffer plane=planes[p].getBuffer();int rowStride=planes[p].getRowStride(),pixelStride=planes[p].getPixelStride();
            for(int row=0;row<ch;row++) {
                int base=row*rowStride;
                if(pixelStride==1){plane.position(base);plane.get(out,offset,cw);offset+=cw;}
                else for(int col=0;col<cw;col++)out[offset++]=plane.get(base+col*pixelStride);
            }
        }
    }
    static byte[] camera(Context context,JSONObject args) throws Exception {
        String id=args.getString("id");int width=args.getInt("width"),height=args.getInt("height");
        CameraManager manager=(CameraManager)context.getSystemService(Context.CAMERA_SERVICE);
        CameraCharacteristics characteristics=manager.getCameraCharacteristics(id);
        android.hardware.camera2.params.StreamConfigurationMap map=characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        Size[] supported=map==null?null:map.getOutputSizes(ImageFormat.YUV_420_888);
        if(supported==null || !Arrays.asList(supported).contains(new Size(width,height)))
            throw new IllegalArgumentException("Requested camera size is not supported by the host");
        if((long)width*height>8*1024*1024)throw new IllegalArgumentException("Camera packet would exceed transport limits");
        if(!cameraUse.tryAcquire())throw new IOException("Host camera is already in use by the guest");
        HandlerThread thread=new HandlerThread("PocketCameraRelay");thread.start();Handler handler=new Handler(thread.getLooper());
        AtomicReference<CameraDevice> camera=new AtomicReference<>();AtomicReference<CameraCaptureSession> session=new AtomicReference<>();
        AtomicReference<byte[]> pixels=new AtomicReference<>();AtomicReference<Exception> failed=new AtomicReference<>();
        AtomicReference<JSONObject> imageInfo=new AtomicReference<>();AtomicReference<TotalCaptureResult> metadata=new AtomicReference<>();
        AtomicBoolean cancelled=new AtomicBoolean();
        CountDownLatch done=new CountDownLatch(2);
        try(ImageReader reader=ImageReader.newInstance(width,height,ImageFormat.YUV_420_888,2)) {
            Runnable fail=()->{done.countDown();done.countDown();};
            reader.setOnImageAvailableListener(source->{
                try(Image image=source.acquireLatestImage()) {
                    if(image==null || pixels.get()!=null)return;
                    ByteArrayOutputStream bytes=new ByteArrayOutputStream();JSONArray planes=new JSONArray();
                    for(Image.Plane plane:image.getPlanes()) {
                        ByteBuffer buffer=plane.getBuffer().duplicate();byte[] data=new byte[buffer.remaining()];buffer.get(data);
                        if((long)bytes.size()+data.length>HostWire.MAX-65536)throw new IOException("Camera planes exceed the transport limit");
                        planes.put(new JSONObject().put("offset",bytes.size()).put("length",data.length)
                            .put("row_stride",plane.getRowStride()).put("pixel_stride",plane.getPixelStride()));
                        bytes.write(data);
                    }
                    JSONObject info=new JSONObject().put("id",id).put("width",width).put("height",height)
                        .put("format",image.getFormat()).put("timestamp_ns",image.getTimestamp()).put("planes",planes)
                        .put("sensor_orientation",characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION))
                        .put("facing",characteristics.get(CameraCharacteristics.LENS_FACING))
                        .put("crop",new JSONArray().put(image.getCropRect().left).put(image.getCropRect().top)
                            .put(image.getCropRect().right).put(image.getCropRect().bottom));
                    imageInfo.set(info);pixels.set(bytes.toByteArray());done.countDown();
                } catch(Exception error){failed.set(error);fail.run();}
            },handler);
            manager.openCamera(id,new CameraDevice.StateCallback() {
                public void onOpened(CameraDevice device) {
                    camera.set(device);
                    if(cancelled.get()){device.close();return;}
                    try {device.createCaptureSession(Collections.singletonList(reader.getSurface()),new CameraCaptureSession.StateCallback() {
                        public void onConfigured(CameraCaptureSession value) {
                            session.set(value);
                            if(cancelled.get()){value.close();return;}
                            try {
                                CaptureRequest.Builder request=device.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE);
                                request.addTarget(reader.getSurface());
                                value.capture(request.build(),new CameraCaptureSession.CaptureCallback() {
                                    public void onCaptureCompleted(CameraCaptureSession s,CaptureRequest r,TotalCaptureResult result){metadata.set(result);done.countDown();}
                                    public void onCaptureFailed(CameraCaptureSession s,CaptureRequest r,CaptureFailure failure){failed.set(new IOException("Host camera capture failed"));fail.run();}
                                },handler);
                            } catch(Exception error){failed.set(error);fail.run();}
                        }
                        public void onConfigureFailed(CameraCaptureSession s){s.close();failed.set(new IOException("Host camera session configuration failed"));fail.run();}
                    },handler);}catch(Exception error){failed.set(error);fail.run();}
                }
                public void onDisconnected(CameraDevice device){device.close();failed.set(new IOException("Host camera disconnected"));fail.run();}
                public void onError(CameraDevice device,int code){device.close();failed.set(new IOException("Host camera error "+code));fail.run();}
            },handler);
            if(!done.await(8,TimeUnit.SECONDS))throw new IOException("Host camera timed out");
            if(failed.get()!=null)throw failed.get();
            if(pixels.get()==null || metadata.get()==null)throw new IOException("Host camera returned incomplete frame");
            JSONObject info=imageInfo.get();TotalCaptureResult result=metadata.get();
            info.put("exposure_ns",result.get(CaptureResult.SENSOR_EXPOSURE_TIME))
                .put("sensitivity_iso",result.get(CaptureResult.SENSOR_SENSITIVITY))
                .put("frame_duration_ns",result.get(CaptureResult.SENSOR_FRAME_DURATION))
                .put("ae_state",result.get(CaptureResult.CONTROL_AE_STATE)).put("af_state",result.get(CaptureResult.CONTROL_AF_STATE));
            ByteArrayOutputStream bytes=new ByteArrayOutputStream();DataOutputStream out=new DataOutputStream(bytes);
            HostWire.write(out,info.toString().getBytes(StandardCharsets.UTF_8));out.write(pixels.get());
            if(bytes.size()>HostWire.MAX)throw new IOException("Host camera frame exceeds transport limit");
            return bytes.toByteArray();
        } finally {
            cancelled.set(true);
            if(session.get()!=null)session.get().close();if(camera.get()!=null)camera.get().close();
            thread.quitSafely();try{thread.join(1000);}finally{cameraUse.release();}
        }
    }
    static byte[] record(Context context,JSONObject args) throws Exception {
        int rate=args.optInt("sample_rate",48000),milliseconds=args.optInt("milliseconds",250);
        if((rate!=16000 && rate!=44100 && rate!=48000) || milliseconds<20 || milliseconds>5000)
            throw new IllegalArgumentException("Unsupported PCM request");
        if(!microphoneUse.tryAcquire())throw new IOException("Host microphone is already in use by the guest");
        AudioRecord record=null;
        try {
            int minimum=AudioRecord.getMinBufferSize(rate,AudioFormat.CHANNEL_IN_MONO,AudioFormat.ENCODING_PCM_16BIT);
            if(minimum<=0)throw new UnsupportedOperationException("Host microphone format unavailable");
            record=new AudioRecord(MediaRecorder.AudioSource.MIC,rate,AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,Math.max(minimum,rate/5));
            if(record.getState()!=AudioRecord.STATE_INITIALIZED)throw new IOException("Host microphone initialization failed");
            int size=(rate*milliseconds/1000)*2,done=0;byte[] pcm=new byte[size];
            record.startRecording();
            if(record.getRecordingState()!=AudioRecord.RECORDSTATE_RECORDING)throw new IOException("Host microphone did not start");
            long started=SystemClock.elapsedRealtimeNanos(),deadline=SystemClock.elapsedRealtime()+milliseconds+2000;
            while(done<size && !Thread.currentThread().isInterrupted()) {
                int count=record.read(pcm,done,size-done,AudioRecord.READ_NON_BLOCKING);
                if(count<0)throw new IOException("Host PCM read failed: "+count);
                done+=count;
                if(SystemClock.elapsedRealtime()>deadline)throw new IOException("Host microphone timed out");
                if(count==0)Thread.sleep(5);
            }
            if(done!=size)throw new InterruptedIOException("Host microphone cancelled");
            JSONObject info=new JSONObject().put("sample_rate",rate).put("channels",1).put("encoding","pcm_s16le")
                .put("frames",size/2).put("start_elapsed_ns",started).put("source","host_microphone");
            ByteArrayOutputStream bytes=new ByteArrayOutputStream();DataOutputStream out=new DataOutputStream(bytes);
            HostWire.write(out,info.toString().getBytes(StandardCharsets.UTF_8));out.write(pcm);return bytes.toByteArray();
        } finally {
            try{if(record!=null){try{record.stop();}catch(IllegalStateException ignored){}record.release();}}
            finally{microphoneUse.release();}
        }
    }
    /** Continuous guest audio output: after the reply, the socket carries only PCM16 until the guest closes it. */
    static void stream(LocalSocket peer,JSONObject args,DataInputStream in,DataOutputStream out) throws Exception {
        int rate=args.getInt("sample_rate"),channels=args.getInt("channels");
        if(rate!=48000 || channels!=2)throw new IllegalArgumentException("Unsupported guest audio stream");
        int mask=AudioFormat.CHANNEL_OUT_STEREO,frame=channels*2;
        int minimum=AudioTrack.getMinBufferSize(rate,mask,AudioFormat.ENCODING_PCM_16BIT);
        if(minimum<=0)throw new UnsupportedOperationException("Host playback format unavailable");
        if(!guestOutput.tryAcquire())throw new IOException("Guest audio output is already streaming");
        AudioTrack track=null;
        try {
            // About 80 ms of host buffering bounds latency; the blocking write paces the guest mixer.
            track=new AudioTrack.Builder().setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(rate).setChannelMask(mask)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setTransferMode(AudioTrack.MODE_STREAM).setBufferSizeInBytes(Math.max(minimum,rate*frame*80/1000)).build();
            if(track.getState()!=AudioTrack.STATE_INITIALIZED)throw new IOException("Host playback initialization failed");
            track.play();
            out.writeInt(0);HostWire.write(out,new JSONObject().put("sample_rate",rate).put("channels",channels)
                .put("buffer_frames",track.getBufferSizeInFrames()).toString().getBytes(StandardCharsets.UTF_8));out.flush();
            // AudioFlinger writes continuously while active and closes the stream on standby.
            peer.setSoTimeout(30000);peer.setReceiveBufferSize(rate*frame/50);
            byte[] buffer=new byte[rate*frame/50];int pending=0;
            while(true) {
                int count=in.read(buffer,pending,buffer.length-pending);
                if(count<0)break;
                pending+=count;int whole=pending-pending%frame;
                if(whole==0)continue;
                int written=track.write(buffer,0,whole,AudioTrack.WRITE_BLOCKING);
                if(written<0)throw new IOException("Host playback write failed: "+written);
                System.arraycopy(buffer,whole,buffer,0,pending-whole);pending-=whole;
            }
        } finally {
            try {if(track!=null){try{track.stop();}catch(IllegalStateException ignored){}track.release();}}
            finally {guestOutput.release();}
        }
    }
    /** Continuous guest audio input: after the reply, the socket carries host microphone PCM16 until the guest closes it. */
    static void capture(LocalSocket peer,JSONObject args,DataOutputStream out) throws Exception {
        int rate=args.getInt("sample_rate"),channels=args.getInt("channels");
        if(rate!=48000 || channels!=1)throw new IllegalArgumentException("Unsupported guest audio input");
        int minimum=AudioRecord.getMinBufferSize(rate,AudioFormat.CHANNEL_IN_MONO,AudioFormat.ENCODING_PCM_16BIT);
        if(minimum<=0)throw new UnsupportedOperationException("Host microphone format unavailable");
        if(!microphoneUse.tryAcquire())throw new IOException("Host microphone is already in use by the guest");
        AudioRecord record=null;
        try {
            record=new AudioRecord(MediaRecorder.AudioSource.MIC,rate,AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,Math.max(minimum,rate*2/5));
            if(record.getState()!=AudioRecord.STATE_INITIALIZED)throw new IOException("Host microphone initialization failed");
            record.startRecording();
            if(record.getRecordingState()!=AudioRecord.RECORDSTATE_RECORDING)throw new IOException("Host microphone did not start");
            out.writeInt(0);HostWire.write(out,new JSONObject().put("sample_rate",rate).put("channels",channels)
                .put("source","host_microphone").toString().getBytes(StandardCharsets.UTF_8));out.flush();
            // The guest reads continuously while recording and closes the stream on standby; the write then fails.
            peer.setSendBufferSize(rate*2/25);
            byte[] buffer=new byte[rate*2/50];
            while(true) {
                int count=record.read(buffer,0,buffer.length,AudioRecord.READ_BLOCKING);
                if(count<0)throw new IOException("Host PCM read failed: "+count);
                out.write(buffer,0,count);out.flush();
            }
        } finally {
            try{if(record!=null){try{record.stop();}catch(IllegalStateException ignored){}record.release();}}
            finally{microphoneUse.release();}
        }
    }
    static JSONObject play(Context context,byte[] packet) throws Exception {
        DataInputStream input=new DataInputStream(new ByteArrayInputStream(packet));
        JSONObject info=new JSONObject(new String(HostWire.read(input,8192),StandardCharsets.UTF_8));
        int rate=info.getInt("sample_rate"),channels=info.getInt("channels"),size=input.available();
        if((rate!=16000 && rate!=44100 && rate!=48000) || (channels!=1 && channels!=2) ||
            !"pcm_s16le".equals(info.getString("encoding")) || size<channels*2 || size%(channels*2)!=0 || size>rate*channels*2*5)
            throw new IllegalArgumentException("Unsupported playback PCM packet");
        byte[] pcm=new byte[size];input.readFully(pcm);
        if(!speakerUse.tryAcquire())throw new IOException("Host playback is busy");
        AudioTrack track=null;
        try {
            int mask=channels==1?AudioFormat.CHANNEL_OUT_MONO:AudioFormat.CHANNEL_OUT_STEREO;
            int minimum=AudioTrack.getMinBufferSize(rate,mask,AudioFormat.ENCODING_PCM_16BIT);
            if(minimum<=0)throw new UnsupportedOperationException("Host playback format unavailable");
            track=new AudioTrack.Builder().setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(rate).setChannelMask(mask)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setTransferMode(AudioTrack.MODE_STREAM).setBufferSizeInBytes(Math.max(minimum,rate*channels/5)).build();
            if(track.getState()!=AudioTrack.STATE_INITIALIZED)throw new IOException("Host playback initialization failed");
            int done=0,frames=size/(channels*2);long deadline=SystemClock.elapsedRealtime()+5000L+frames*1000L/rate;
            track.play();
            while(done<size && !Thread.currentThread().isInterrupted()) {
                int count=track.write(pcm,done,size-done,AudioTrack.WRITE_NON_BLOCKING);
                if(count<0)throw new IOException("Host playback write failed: "+count);done+=count;
                if(SystemClock.elapsedRealtime()>deadline)throw new IOException("Host playback timed out");
                if(count==0)Thread.sleep(5);
            }
            while(Integer.toUnsignedLong(track.getPlaybackHeadPosition())<frames) {
                if(Thread.currentThread().isInterrupted())throw new InterruptedIOException("Host playback cancelled");
                if(SystemClock.elapsedRealtime()>deadline)throw new IOException("Host playback did not drain");
                Thread.sleep(10);
            }
            return new JSONObject().put("frames",frames).put("sample_rate",rate).put("channels",channels).put("source","host_audio_track");
        } finally {
            try {if(track!=null){try{track.pause();track.flush();}catch(IllegalStateException ignored){}track.release();}}
            finally {speakerUse.release();}
        }
    }
}
