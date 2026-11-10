// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketav32test;

import android.content.Context;
import android.graphics.ImageFormat;
import android.hardware.camera2.*;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.*;
import android.os.*;
import android.util.Size;
import android.view.Surface;
import java.io.*;
import java.nio.ByteBuffer;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import org.json.*;

/** Public Camera2/microphone -> MediaRecorder -> MP4 -> MediaCodec signal proof. */
final class CameraRecording {
    private static void check(boolean value,String message)throws IOException {if(!value)throw new IOException(message);}
    private static final class Sample {
        final byte[] bytes;final long pts;
        Sample(byte[] bytes,long pts){this.bytes=bytes;this.pts=pts;}
    }
    private static JSONObject movie(File file,int width,int height)throws Exception {
        check(file.length()>100 && file.length()<8*1024*1024,"Recorded movie size is invalid");
        MediaExtractor extractor=new MediaExtractor();List<Sample> first=new ArrayList<>();MediaFormat format;JSONObject audio;int samples=0;long previous=-1;boolean sync=false;
        try {
            extractor.setDataSource(file.getPath());check(extractor.getTrackCount()==2,"Expected camera video and microphone audio tracks");int videoTrack=-1,audioTrack=-1;
            for(int i=0;i<2;i++) {
                String mime=extractor.getTrackFormat(i).getString(MediaFormat.KEY_MIME);
                if(MediaFormat.MIMETYPE_VIDEO_AVC.equals(mime))videoTrack=i;
                if(MediaFormat.MIMETYPE_AUDIO_AAC.equals(mime))audioTrack=i;
            }
            check(videoTrack>=0 && audioTrack>=0 && videoTrack!=audioTrack,"Recorded AVC/AAC tracks missing");format=extractor.getTrackFormat(videoTrack);
            check(MediaFormat.MIMETYPE_VIDEO_AVC.equals(format.getString(MediaFormat.KEY_MIME)) && format.getInteger(MediaFormat.KEY_WIDTH)==width
                && format.getInteger(MediaFormat.KEY_HEIGHT)==height,"Recorded video format differs");
            long duration=format.getLong(MediaFormat.KEY_DURATION);check(duration>=1000000 && duration<=20000000,"Recorded duration is invalid");
            audio=CameraRecordingAudio.read(extractor,audioTrack);extractor.selectTrack(videoTrack);extractor.seekTo(0,MediaExtractor.SEEK_TO_PREVIOUS_SYNC);
            ByteBuffer buffer=ByteBuffer.allocateDirect(1024*1024);long started=SystemClock.elapsedRealtime();
            while(true) {
                check(SystemClock.elapsedRealtime()-started<15000,"MP4 extraction timed out");buffer.clear();int count=extractor.readSampleData(buffer,0);if(count<0)break;
                long pts=extractor.getSampleTime();check(count>0 && count<=buffer.capacity() && pts>=0 && pts>previous,"Recorded packet timestamp/size differs");
                if(samples==0)sync=(extractor.getSampleFlags()&MediaExtractor.SAMPLE_FLAG_SYNC)!=0;
                if(first.size()<12){byte[] bytes=new byte[count];buffer.position(0);buffer.get(bytes);first.add(new Sample(bytes,pts));}
                previous=pts;check(++samples<=600,"Too many movie samples");extractor.advance();
            }
            check(first.size()==12 && sync,"Recorded movie lacks twelve frames or initial sync packet");
            long overlap=Math.min(previous,audio.getLong("lastPresentationTimeUs"))-Math.max(first.get(0).pts,audio.getLong("firstPresentationTimeUs"));
            check(overlap>=500000,"Recorded audio/video timelines do not overlap");audio.put("videoOverlapUs",overlap);
        }finally{extractor.release();}
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT,MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
        MediaCodec decoder=MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);boolean active=false,inputEnd=false,outputEnd=false;int next=0;
        JSONArray pixels=new JSONArray();Set<Long> seen=new HashSet<>();String name=decoder.getName();long started=SystemClock.elapsedRealtime();
        try {
            decoder.configure(format,null,null,0);decoder.start();active=true;MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();
            while(!outputEnd) {
                check(SystemClock.elapsedRealtime()-started<30000,"Recorded video decode timed out");
                if(!inputEnd) {
                    int index=decoder.dequeueInputBuffer(10000);
                    if(index>=0) {
                        if(next==first.size()){decoder.queueInputBuffer(index,0,0,first.get(next-1).pts+33333,MediaCodec.BUFFER_FLAG_END_OF_STREAM);inputEnd=true;}
                        else {
                            Sample sample=first.get(next++);ByteBuffer buffer=decoder.getInputBuffer(index);check(buffer!=null && buffer.capacity()>=sample.bytes.length,"Movie decoder input too small");
                            buffer.clear();buffer.put(sample.bytes);decoder.queueInputBuffer(index,0,sample.bytes.length,sample.pts,0);
                        }
                    }
                }
                int index=decoder.dequeueOutputBuffer(info,10000);
                if(index>=0)try {
                    if(info.size>0) {
                        check(first.stream().anyMatch(sample->sample.pts==info.presentationTimeUs) && seen.add(info.presentationTimeUs),"Unexpected/duplicate recorded frame timestamp");
                        try(Image image=decoder.getOutputImage(index)) {
                            check(image!=null && image.getFormat()==ImageFormat.YUV_420_888 && image.getCropRect().width()==width
                                && image.getCropRect().height()==height && image.getPlanes().length==3,"Movie decoder output is missing");
                            Image.Plane plane=image.getPlanes()[0];ByteBuffer buffer=plane.getBuffer();int min=255,max=0,count=0;long sum=0;
                            android.graphics.Rect crop=image.getCropRect();int origin=buffer.position();
                            for(int y=0;y<height;y+=4)for(int x=0;x<width;x+=4) {
                                int value=buffer.get(origin+(y+crop.top)*plane.getRowStride()+(x+crop.left)*plane.getPixelStride())&255;
                                min=Math.min(min,value);max=Math.max(max,value);sum+=value;count++;
                            }
                            check(max-min>5,"Recorded video has no camera luma variation");
                            pixels.put(new JSONObject().put("presentationTimeUs",info.presentationTimeUs).put("minLuma",min).put("maxLuma",max)
                                .put("meanLuma",sum/(double)count).put("sampledPixels",count));
                        }
                    }
                    outputEnd=(info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0;
                }finally{decoder.releaseOutputBuffer(index,false);}
            }
            check(inputEnd && seen.size()==12 && pixels.length()==12,"Recorded decoder frames are incomplete");
        }finally{try{if(active)decoder.stop();}finally{decoder.release();}}
        java.security.MessageDigest digest=java.security.MessageDigest.getInstance("SHA-256");
        try(InputStream input=new FileInputStream(file)){byte[] buffer=new byte[16384];int n;while((n=input.read(buffer))!=-1)digest.update(buffer,0,n);}
        StringBuilder hash=new StringBuilder();for(byte value:digest.digest())hash.append(String.format("%02x",value&255));
        return new JSONObject().put("bytes",file.length()).put("sha256",hash.toString()).put("container","mp4").put("mime",MediaFormat.MIMETYPE_VIDEO_AVC)
            .put("width",width).put("height",height).put("durationUs",format.getLong(MediaFormat.KEY_DURATION)).put("packets",samples).put("initialSync",sync)
            .put("firstPresentationTimeUs",first.get(0).pts).put("lastPresentationTimeUs",previous)
            .put("decoder",name).put("decodedFrames",pixels.length()).put("pixels",pixels).put("endOfStream",outputEnd).put("audio",audio);
    }
    static JSONObject run(Context context,String id)throws Exception {
        CameraManager manager=context.getSystemService(CameraManager.class);String[] ids=manager.getCameraIdList();check(Arrays.asList(ids).contains(id),"Recording camera is not advertised");
        StreamConfigurationMap map=manager.getCameraCharacteristics(id).get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);check(map!=null,"Recording stream map missing");
        Size[] sizes=map.getOutputSizes(MediaRecorder.class);check(sizes!=null && sizes.length>0,"Camera does not advertise MediaRecorder outputs");
        Size size=Arrays.stream(sizes).filter(s->s.getWidth()>=160 && s.getHeight()>=120).min(Comparator.comparingLong(s->(long)s.getWidth()*s.getHeight()))
            .orElseThrow(()->new IOException("No usable recorder output size"));
        File file=File.createTempFile("camera-record32-",".mp4",context.getCacheDir());HandlerThread thread=new HandlerThread("CameraRecorder32");thread.start();Handler handler=new Handler(thread.getLooper());
        AtomicReference<CameraDevice> camera=new AtomicReference<>();AtomicReference<CameraCaptureSession> session=new AtomicReference<>();AtomicReference<Throwable> failure=new AtomicReference<>();
        AtomicBoolean cancelled=new AtomicBoolean(),draining=new AtomicBoolean();AtomicInteger captures=new AtomicInteger(),flushed=new AtomicInteger();
        CountDownLatch opened=new CountDownLatch(1),configured=new CountDownLatch(1),closed=new CountDownLatch(1),idleAfterStop=new CountDownLatch(1);
        MediaRecorder recorder=null;Surface surface=null;
        Runnable failed=()->{opened.countDown();configured.countDown();};
        try {
            recorder=new MediaRecorder(context);
            recorder.setOnErrorListener((value,what,extra)->failure.compareAndSet(null,new IOException("Recorder error "+what+"/"+extra)));
            recorder.setAudioSource(MediaRecorder.AudioSource.MIC);recorder.setVideoSource(MediaRecorder.VideoSource.SURFACE);recorder.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4);
            recorder.setAudioEncoder(MediaRecorder.AudioEncoder.AAC);recorder.setAudioSamplingRate(48000);recorder.setAudioChannels(1);recorder.setAudioEncodingBitRate(64000);
            recorder.setVideoEncoder(MediaRecorder.VideoEncoder.H264);recorder.setVideoSize(size.getWidth(),size.getHeight());recorder.setVideoFrameRate(30);recorder.setVideoEncodingBitRate(256000);
            recorder.setOutputFile(file.getPath());recorder.prepare();surface=recorder.getSurface();check(surface!=null && surface.isValid(),"Recorder input Surface missing");
            manager.openCamera(id,new CameraDevice.StateCallback() {
                public void onOpened(CameraDevice value){camera.set(value);if(cancelled.get())value.close();opened.countDown();}
                public void onDisconnected(CameraDevice value){value.close();if(!cancelled.get()){failure.compareAndSet(null,new IOException("Recording camera disconnected"));failed.run();}}
                public void onError(CameraDevice value,int error){value.close();if(!cancelled.get()){failure.compareAndSet(null,new IOException("Recording camera error "+error));failed.run();}}
                public void onClosed(CameraDevice value){closed.countDown();}
            },handler);
            check(opened.await(15,TimeUnit.SECONDS),"Recording camera open timed out");if(failure.get()!=null)throw new IOException("Recorder camera open failed",failure.get());check(camera.get()!=null,"No recording camera");
            camera.get().createCaptureSession(Collections.singletonList(surface),new CameraCaptureSession.StateCallback() {
                public void onConfigured(CameraCaptureSession value){session.set(value);if(cancelled.get())value.close();configured.countDown();}
                public void onConfigureFailed(CameraCaptureSession value){value.close();failure.compareAndSet(null,new IOException("Recorder capture session failed"));failed.run();}
                public void onReady(CameraCaptureSession value){if(draining.get())idleAfterStop.countDown();}
            },handler);
            check(configured.await(15,TimeUnit.SECONDS),"Recorder session timed out");if(failure.get()!=null)throw new IOException("Recorder session failed",failure.get());check(session.get()!=null,"No recording session");
            CaptureRequest.Builder request=camera.get().createCaptureRequest(CameraDevice.TEMPLATE_RECORD);request.addTarget(surface);
            session.get().setRepeatingRequest(request.build(),new CameraCaptureSession.CaptureCallback() {
                public void onCaptureCompleted(CameraCaptureSession s,CaptureRequest r,TotalCaptureResult result){captures.incrementAndGet();}
                public void onCaptureFailed(CameraCaptureSession s,CaptureRequest r,CaptureFailure error) {
                    // abortCaptures reports pending requests as REASON_FLUSHED.
                    // Only our deliberate shutdown may accept that cancellation.
                    if(draining.get() && error.getReason()==CaptureFailure.REASON_FLUSHED){flushed.incrementAndGet();return;}
                    if(!cancelled.get())failure.compareAndSet(null,new IOException("Recording capture failed "+error.getReason()));
                }
            },handler);
            recorder.start();long started=SystemClock.elapsedRealtime();
            while(SystemClock.elapsedRealtime()-started<4000 || captures.get()<12) {
                if(failure.get()!=null)throw new IOException("Recording failed",failure.get());check(SystemClock.elapsedRealtime()-started<12000,"Recording frames timed out");Thread.sleep(50);
            }
            draining.set(true);session.get().stopRepeating();session.get().abortCaptures();
            check(idleAfterStop.await(10,TimeUnit.SECONDS),"Recording camera did not become ready after abort");recorder.stop();
            if(failure.get()!=null)throw new IOException("Recorder stop failed",failure.get());
            JSONObject movie=movie(file,size.getWidth(),size.getHeight());
            if(failure.get()!=null)throw new IOException("Recorder finalization failed",failure.get());
            return movie.put("cameraId",id).put("advertisedCameras",new JSONArray(Arrays.asList(ids))).put("captureCompletions",captures.get()).put("requestedFrameRate",30)
                .put("shutdownFlushedFrames",flushed.get());
        }finally {
            cancelled.set(true);
            try {
                if(session.get()!=null)session.get().close();if(camera.get()!=null){camera.get().close();closed.await(5,TimeUnit.SECONDS);}
            }finally {
                try{if(recorder!=null)recorder.release();}finally{if(surface!=null)surface.release();thread.quitSafely();thread.join(5000);boolean deleted=file.delete();check(deleted || !file.exists(),"Temporary movie cleanup failed");check(!thread.isAlive(),"Recorder callback thread failed to stop");}
            }
        }
    }
}
