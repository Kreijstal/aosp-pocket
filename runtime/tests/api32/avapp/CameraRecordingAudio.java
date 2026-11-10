// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketav32test;

import android.media.*;
import android.os.SystemClock;
import java.io.IOException;
import java.nio.*;
import java.util.*;
import org.json.*;

/** Decode captured AAC from the same MP4 as the camera video, using public APIs. */
final class CameraRecordingAudio {
    private static void check(boolean value,String message)throws IOException {if(!value)throw new IOException(message);}
    private static final class Sample {
        final byte[] bytes;final long pts;
        Sample(byte[] bytes,long pts){this.bytes=bytes;this.pts=pts;}
    }
    static JSONObject read(MediaExtractor extractor,int track)throws Exception {
        MediaFormat format=extractor.getTrackFormat(track);
        check(MediaFormat.MIMETYPE_AUDIO_AAC.equals(format.getString(MediaFormat.KEY_MIME)) && format.getInteger(MediaFormat.KEY_SAMPLE_RATE)==48000
            && format.getInteger(MediaFormat.KEY_CHANNEL_COUNT)==1,"Recorded AAC format differs");
        long duration=format.getLong(MediaFormat.KEY_DURATION);check(duration>=1000000 && duration<=20000000,"Recorded AAC duration differs");
        List<Sample> first=new ArrayList<>();long previous=-1,firstPts=-1;int packets=0,encodedBytes=0;
        extractor.selectTrack(track);
        try {
            extractor.seekTo(0,MediaExtractor.SEEK_TO_PREVIOUS_SYNC);ByteBuffer buffer=ByteBuffer.allocateDirect(65536);long started=SystemClock.elapsedRealtime();
            while(true) {
                check(SystemClock.elapsedRealtime()-started<15000,"Recorded AAC extraction timed out");buffer.clear();int count=extractor.readSampleData(buffer,0);if(count<0)break;
                long pts=extractor.getSampleTime();check(count>0 && count<=buffer.capacity() && pts>=0 && pts>previous,"Recorded AAC packet timestamp/size differs");
                if(firstPts<0)firstPts=pts;
                if(first.size()<12){byte[] bytes=new byte[count];buffer.position(0);buffer.get(bytes);first.add(new Sample(bytes,pts));}
                previous=pts;encodedBytes=Math.addExact(encodedBytes,count);check(++packets<=1200 && encodedBytes<2*1024*1024,"Recorded AAC exceeds bounds");extractor.advance();
            }
            check(first.size()==12,"Recorded AAC lacks twelve packets");
        }finally{extractor.unselectTrack(track);}
        MediaCodec decoder=MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_AAC);boolean active=false,inputEnd=false,outputEnd=false;int next=0,count=0,outputs=0,min=32767,max=-32768;
        double squares=0;String name=decoder.getName();JSONArray timestamps=new JSONArray();long started=SystemClock.elapsedRealtime(),lastOutput=-1;
        try {
            decoder.configure(format,null,null,0);decoder.start();active=true;MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();
            while(!outputEnd) {
                check(SystemClock.elapsedRealtime()-started<30000,"Recorded AAC decode timed out");
                if(!inputEnd) {
                    int index=decoder.dequeueInputBuffer(10000);
                    if(index>=0) {
                        if(next==first.size()){decoder.queueInputBuffer(index,0,0,first.get(next-1).pts+21333,MediaCodec.BUFFER_FLAG_END_OF_STREAM);inputEnd=true;}
                        else {
                            Sample sample=first.get(next++);ByteBuffer buffer=decoder.getInputBuffer(index);check(buffer!=null && buffer.capacity()>=sample.bytes.length,"AAC decoder input too small");
                            buffer.clear();buffer.put(sample.bytes);decoder.queueInputBuffer(index,0,sample.bytes.length,sample.pts,0);
                        }
                    }
                }
                int index=decoder.dequeueOutputBuffer(info,10000);
                if(index>=0)try {
                    if(info.size>0) {
                        MediaFormat output=decoder.getOutputFormat(index);
                        check(output.getInteger(MediaFormat.KEY_SAMPLE_RATE)==48000 && output.getInteger(MediaFormat.KEY_CHANNEL_COUNT)==1
                            && (!output.containsKey(MediaFormat.KEY_PCM_ENCODING) || output.getInteger(MediaFormat.KEY_PCM_ENCODING)==AudioFormat.ENCODING_PCM_16BIT),"Recorded AAC PCM format differs");
                        check(info.presentationTimeUs>=0 && info.presentationTimeUs>lastOutput && info.size%2==0,"Recorded PCM timestamp/size differs");
                        ByteBuffer raw=decoder.getOutputBuffer(index);check(raw!=null && info.offset>=0 && info.offset+info.size<=raw.capacity(),"Recorded PCM buffer missing");
                        ByteBuffer buffer=raw.duplicate().order(ByteOrder.LITTLE_ENDIAN);buffer.position(info.offset);buffer.limit(info.offset+info.size);
                        while(buffer.remaining()>=2){short value=buffer.getShort();squares+=(double)value*value;min=Math.min(min,value);max=Math.max(max,value);count++;}
                        check(count<=16384 && ++outputs<=16,"Recorded PCM exceeds bounds");timestamps.put(info.presentationTimeUs);lastOutput=info.presentationTimeUs;
                    }
                    outputEnd=(info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0;
                }finally{decoder.releaseOutputBuffer(index,false);}
            }
            double rms=Math.sqrt(squares/Math.max(1,count));
            check(inputEnd && count>=8192 && outputs>=8 && min<0 && max>0 && max-min>100 && rms>50 && rms<32767,"Recorded AAC lacks a usable microphone signal");
            return new JSONObject().put("mime",MediaFormat.MIMETYPE_AUDIO_AAC).put("sampleRate",48000).put("channels",1).put("durationUs",duration)
                .put("packets",packets).put("encodedBytes",encodedBytes).put("firstPresentationTimeUs",firstPts).put("lastPresentationTimeUs",previous)
                .put("decoder",name).put("decodedSamples",count).put("decoderOutputs",outputs).put("decodedPresentationTimesUs",timestamps)
                .put("minPcm",min).put("maxPcm",max).put("rms",rms).put("endOfStream",outputEnd);
        }finally{try{if(active)decoder.stop();}finally{decoder.release();}}
    }
}
