// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketcodec32test;
import android.graphics.Rect;
import android.media.*;
import android.os.SystemClock;
import java.io.ByteArrayOutputStream;
import java.nio.*;
import java.util.*;
import org.json.*;

/** Synthetic AAC/PCM and AVC/YUV payloads detect missing codecs and incorrect buffer transport. */
final class CodecRoundTrip {
    interface Stage {void update(String value)throws Exception;}
    interface Fill {int input(MediaCodec codec,int index,int frame)throws Exception;}
    interface Read {void output(MediaCodec codec,int index,MediaCodec.BufferInfo info)throws Exception;}
    private static final class Packet {
        final byte[] data;final long pts;
        Packet(byte[] data,long pts){this.data=data;this.pts=pts;}
    }
    private static final class Encoded {
        final List<Packet> packets=new ArrayList<>();final Map<String,byte[]> csd=new LinkedHashMap<>();
        String name;long elapsed;int bytes;
        void configure(MediaFormat format) {
            for(Map.Entry<String,byte[]> entry:csd.entrySet())format.setByteBuffer(entry.getKey(),ByteBuffer.wrap(entry.getValue()));
        }
    }
    private static void check(boolean value,String message){if(!value)throw new IllegalStateException(message);}
    private static byte[] bytes(ByteBuffer buffer,int offset,int count) {
        check(buffer!=null && offset>=0 && count>=0 && offset+count<=buffer.capacity(),"Invalid output byte range");
        ByteBuffer copy=buffer.duplicate();copy.position(offset);copy.limit(offset+count);byte[] result=new byte[count];copy.get(result);return result;
    }
    private static Encoded encode(String mime,MediaFormat format,int frames,long frameMicros,Fill fill)throws Exception {
        MediaCodec codec=MediaCodec.createEncoderByType(mime);Encoded result=new Encoded();long started=SystemClock.elapsedRealtime();boolean active=false;
        try {
            result.name=codec.getName();codec.configure(format,null,null,MediaCodec.CONFIGURE_FLAG_ENCODE);codec.start();active=true;
            MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();boolean inputEnd=false,outputEnd=false;int frame=0;
            while(!outputEnd) {
                check(SystemClock.elapsedRealtime()-started<30000,"Encoder deadline exceeded: "+mime);
                if(!inputEnd) {
                    int index=codec.dequeueInputBuffer(10000);
                    if(index>=0) {
                        if(frame==frames){codec.queueInputBuffer(index,0,0,frame*frameMicros,MediaCodec.BUFFER_FLAG_END_OF_STREAM);inputEnd=true;}
                        else {int count=fill.input(codec,index,frame);codec.queueInputBuffer(index,0,count,frame*frameMicros,0);frame++;}
                    }
                }
                int index=codec.dequeueOutputBuffer(info,10000);
                if(index==MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    MediaFormat output=codec.getOutputFormat();
                    for(int i=0;i<3;i++) {
                        String key="csd-"+i;ByteBuffer value=output.getByteBuffer(key);
                        if(value!=null){check(value.remaining()<16384,"Oversized codec configuration");result.csd.put(key,bytes(value,value.position(),value.remaining()));}
                    }
                }else if(index>=0) {
                    try {
                        if(info.size>0) {
                            byte[] packet=bytes(codec.getOutputBuffer(index),info.offset,info.size);
                            if((info.flags&MediaCodec.BUFFER_FLAG_CODEC_CONFIG)!=0) {
                                if(result.csd.isEmpty())result.csd.put("csd-0",packet);
                            }else{result.packets.add(new Packet(packet,info.presentationTimeUs));result.bytes+=packet.length;}
                            check(result.bytes<4*1024*1024 && result.packets.size()<frames*3+16,"Encoder output exceeds fixture bounds");
                        }
                        outputEnd=(info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0;
                    }finally{codec.releaseOutputBuffer(index,false);}
                }
            }
            check(inputEnd && frame==frames && result.bytes>0 && !result.packets.isEmpty() && !result.csd.isEmpty(),"Incomplete encoded stream: "+mime);
            result.elapsed=SystemClock.elapsedRealtime()-started;return result;
        }finally{try{if(active)codec.stop();}finally{codec.release();}}
    }
    private static JSONObject decode(String mime,MediaFormat format,Encoded input,Read read)throws Exception {
        MediaCodec codec=MediaCodec.createDecoderByType(mime);long started=SystemClock.elapsedRealtime();int packet=0,outputs=0;boolean inputEnd=false,outputEnd=false,active=false;
        try {
            String name=codec.getName();input.configure(format);codec.configure(format,null,null,0);codec.start();active=true;
            MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();
            while(!outputEnd) {
                check(SystemClock.elapsedRealtime()-started<30000,"Decoder deadline exceeded: "+mime);
                if(!inputEnd) {
                    int index=codec.dequeueInputBuffer(10000);
                    if(index>=0) {
                        if(packet==input.packets.size()){codec.queueInputBuffer(index,0,0,input.packets.get(packet-1).pts+100000,MediaCodec.BUFFER_FLAG_END_OF_STREAM);inputEnd=true;}
                        else {
                            Packet value=input.packets.get(packet++);ByteBuffer buffer=codec.getInputBuffer(index);check(buffer!=null && buffer.capacity()>=value.data.length,"Decoder input too small");
                            buffer.clear();buffer.put(value.data);codec.queueInputBuffer(index,0,value.data.length,value.pts,0);
                        }
                    }
                }
                int index=codec.dequeueOutputBuffer(info,10000);
                if(index>=0) {
                    try{if(info.size>0){read.output(codec,index,info);outputs++;}outputEnd=(info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0;}
                    finally{codec.releaseOutputBuffer(index,false);}
                }
            }
            check(inputEnd && outputs>0,"Incomplete decoded stream: "+mime);
            return new JSONObject().put("name",name).put("outputs",outputs).put("elapsedMillis",SystemClock.elapsedRealtime()-started).put("format",codec.getOutputFormat().toString());
        }finally{try{if(active)codec.stop();}finally{codec.release();}}
    }
    private static JSONObject audio(Stage stage)throws Exception {
        final int rate=48000,frames=10,samples=1024;String mime=MediaFormat.MIMETYPE_AUDIO_AAC;
        MediaFormat format=MediaFormat.createAudioFormat(mime,rate,1);format.setInteger(MediaFormat.KEY_AAC_PROFILE,MediaCodecInfo.CodecProfileLevel.AACObjectLC);
        format.setInteger(MediaFormat.KEY_BIT_RATE,64000);format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE,4096);
        stage.update("aac-encode");Encoded encoded=encode(mime,format,frames,samples*1000000L/rate,(codec,index,frame)->{
            ByteBuffer buffer=codec.getInputBuffer(index);check(buffer!=null && buffer.capacity()>=samples*2,"AAC input too small");buffer.clear();buffer.order(ByteOrder.LITTLE_ENDIAN);
            for(int i=0;i<samples;i++)buffer.putShort((short)Math.round(8000*Math.sin(2*Math.PI*440*(frame*samples+i)/rate)));
            return samples*2;
        });
        check(encoded.bytes<frames*samples,"AAC output was not compressed");
        stage.update("aac-decode");ByteArrayOutputStream pcm=new ByteArrayOutputStream();
        JSONObject decoder=decode(mime,MediaFormat.createAudioFormat(mime,rate,1),encoded,(codec,index,info)->{
            MediaFormat output=codec.getOutputFormat(index);check(output.getInteger(MediaFormat.KEY_SAMPLE_RATE)==rate && output.getInteger(MediaFormat.KEY_CHANNEL_COUNT)==1,"Unexpected decoded PCM format");
            check(!output.containsKey(MediaFormat.KEY_PCM_ENCODING) || output.getInteger(MediaFormat.KEY_PCM_ENCODING)==android.media.AudioFormat.ENCODING_PCM_16BIT,"Unexpected PCM encoding");
            check(pcm.size()+info.size<1024*1024,"PCM output exceeds fixture bounds");pcm.write(bytes(codec.getOutputBuffer(index),info.offset,info.size));
        });
        byte[] raw=pcm.toByteArray();check(raw.length%2==0,"Odd PCM byte count");int count=raw.length/2;
        check(count>=frames*samples-2048 && count<=frames*samples+4096,"Decoded AAC duration differs");
        ByteBuffer values=ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN);int start=2048,end=Math.min(count-1024,start+4096),crossings=0;double square=0;short previous=values.getShort(start*2);
        check(end-start>=2048,"Too little decoded audio");
        for(int i=start;i<end;i++){short sample=values.getShort(i*2);square+=(double)sample*sample;if((sample>=0)!=(previous>=0))crossings++;previous=sample;}
        double rms=Math.sqrt(square/(end-start)),frequency=crossings*rate/(2.0*(end-start));
        check(rms>3000 && rms<9000 && Math.abs(frequency-440)<50,"AAC signal amplitude/frequency differs");
        return new JSONObject().put("encoder",encoded.name).put("decoder",decoder).put("encodedBytes",encoded.bytes).put("packets",encoded.packets.size())
            .put("sampleRate",rate).put("channels",1).put("inputSamples",frames*samples).put("decodedSamples",count).put("rms",rms).put("frequencyHz",frequency).put("encodeMillis",encoded.elapsed);
    }
    private static double planeMean(Image image,int plane) {
        Rect crop=image.getCropRect();int scale=plane==0?1:2,width=crop.width()/scale,height=crop.height()/scale;
        Image.Plane p=image.getPlanes()[plane];ByteBuffer data=p.getBuffer();int origin=data.position(),row=p.getRowStride(),pixel=p.getPixelStride();long sum=0;
        for(int y=0;y<height;y++)for(int x=0;x<width;x++)sum+=data.get(origin+(y+crop.top/scale)*row+(x+crop.left/scale)*pixel)&255;
        return sum/(double)(width*height);
    }
    private static JSONObject video(Stage stage)throws Exception {
        final int width=64,height=64,frames=12;String mime=MediaFormat.MIMETYPE_VIDEO_AVC;
        MediaFormat format=MediaFormat.createVideoFormat(mime,width,height);format.setInteger(MediaFormat.KEY_COLOR_FORMAT,MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
        format.setInteger(MediaFormat.KEY_BIT_RATE,128000);format.setInteger(MediaFormat.KEY_FRAME_RATE,10);format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL,1);format.setInteger(MediaFormat.KEY_MAX_B_FRAMES,0);
        stage.update("avc-encode");Encoded encoded=encode(mime,format,frames,100000,(codec,index,frame)->{
            try(Image image=codec.getInputImage(index)) {
                check(image!=null && image.getWidth()==width && image.getHeight()==height && image.getPlanes().length==3,"Missing flexible YUV encoder input");
                for(int n=0;n<3;n++) {
                    Image.Plane plane=image.getPlanes()[n];ByteBuffer data=plane.getBuffer();int origin=data.position(),w=n==0?width:width/2,h=n==0?height:height/2,value=n==0?32+frame*5:n==1?96:160;
                    for(int y=0;y<h;y++)for(int x=0;x<w;x++)data.put(origin+y*plane.getRowStride()+x*plane.getPixelStride(),(byte)value);
                }
            }
            return width*height*3/2;
        });
        check(encoded.bytes<frames*width*height*3/4,"AVC output was not compressed");
        stage.update("avc-decode");MediaFormat output=MediaFormat.createVideoFormat(mime,width,height);output.setInteger(MediaFormat.KEY_COLOR_FORMAT,MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
        Set<Integer> seen=new HashSet<>();JSONArray pixels=new JSONArray();
        JSONObject decoder=decode(mime,output,encoded,(codec,index,info)->{
            int frame=(int)(info.presentationTimeUs/100000);check(info.presentationTimeUs%100000==0 && frame>=0 && frame<frames && seen.add(frame),"Unexpected/duplicate video timestamp");
            try(Image image=codec.getOutputImage(index)) {
                check(image!=null && image.getPlanes().length==3 && image.getCropRect().width()==width && image.getCropRect().height()==height,"Missing flexible YUV decoder output");
                double y=planeMean(image,0),u=planeMean(image,1),v=planeMean(image,2);
                check(Math.abs(y-(32+frame*5))<=12 && Math.abs(u-96)<=12 && Math.abs(v-160)<=12,"Decoded AVC pixels differ");
                pixels.put(new JSONObject().put("frame",frame).put("y",y).put("u",u).put("v",v));
            }
        });
        check(seen.size()==frames,"Video frames lost");
        return new JSONObject().put("encoder",encoded.name).put("decoder",decoder).put("encodedBytes",encoded.bytes).put("packets",encoded.packets.size())
            .put("width",width).put("height",height).put("inputFrames",frames).put("decodedFrames",seen.size()).put("pixels",pixels).put("encodeMillis",encoded.elapsed);
    }
    private static JSONObject surfaceVideo(Stage stage)throws Exception {
        final int width=64,height=64,frames=12;String mime=MediaFormat.MIMETYPE_VIDEO_AVC;
        MediaFormat format=MediaFormat.createVideoFormat(mime,width,height);
        format.setInteger(MediaFormat.KEY_COLOR_FORMAT,MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        format.setInteger(MediaFormat.KEY_BIT_RATE,128000);format.setInteger(MediaFormat.KEY_FRAME_RATE,10);
        format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL,1);format.setInteger(MediaFormat.KEY_MAX_B_FRAMES,0);
        stage.update("surface-avc-encode");MediaCodec codec=MediaCodec.createEncoderByType(mime);
        Encoded encoded=new Encoded();android.view.Surface input=null;CodecSurface egl=null;boolean active=false,ended=false;String renderer=null,vendor=null;
        long started=SystemClock.elapsedRealtime();int submitted=0;boolean drawNext=true;
        try {
            encoded.name=codec.getName();codec.configure(format,null,null,MediaCodec.CONFIGURE_FLAG_ENCODE);
            input=codec.createInputSurface();codec.start();active=true;egl=new CodecSurface(input);renderer=egl.renderer;vendor=egl.vendor;
            MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();
            while(!ended) {
                check(SystemClock.elapsedRealtime()-started<30000,"Surface encoder deadline exceeded");
                if(drawNext && submitted<frames) {egl.draw(submitted++,width,height);if(submitted==frames)codec.signalEndOfInputStream();}
                int index=codec.dequeueOutputBuffer(info,10000);drawNext=index==MediaCodec.INFO_TRY_AGAIN_LATER;
                if(index==MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    MediaFormat output=codec.getOutputFormat();
                    for(int i=0;i<3;i++) {
                        String key="csd-"+i;ByteBuffer value=output.getByteBuffer(key);
                        if(value!=null){check(value.remaining()<16384,"Oversized Surface codec configuration");encoded.csd.put(key,bytes(value,value.position(),value.remaining()));}
                    }
                }else if(index>=0) {
                    try {
                        if(info.size>0) {
                            byte[] packet=bytes(codec.getOutputBuffer(index),info.offset,info.size);
                            if((info.flags&MediaCodec.BUFFER_FLAG_CODEC_CONFIG)!=0) {if(encoded.csd.isEmpty())encoded.csd.put("csd-0",packet);}
                            else {encoded.packets.add(new Packet(packet,info.presentationTimeUs));encoded.bytes+=packet.length;}
                            check(encoded.bytes<4*1024*1024 && encoded.packets.size()<frames*3+16,"Surface encoder output exceeds bounds");
                        }
                        ended=(info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0;
                    }finally{codec.releaseOutputBuffer(index,false);}
                }
            }
            check(submitted==frames && !encoded.csd.isEmpty() && encoded.bytes>0 && encoded.bytes<frames*width*height*3/4,"Incomplete/uncompressed Surface stream");
            encoded.elapsed=SystemClock.elapsedRealtime()-started;
        }finally {
            try{if(egl!=null)egl.close();}finally{try{if(input!=null)input.release();}finally{try{if(active)codec.stop();}finally{codec.release();}}}
        }
        stage.update("surface-avc-decode");MediaFormat output=MediaFormat.createVideoFormat(mime,width,height);
        output.setInteger(MediaFormat.KEY_COLOR_FORMAT,MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
        Set<Integer> seen=new HashSet<>();JSONArray pixels=new JSONArray();
        JSONObject decoder=decode(mime,output,encoded,(value,index,info)->{
            int frame=(int)(info.presentationTimeUs/100000)-1;
            check(info.presentationTimeUs%100000==0 && frame>=0 && frame<frames && seen.add(frame),"Unexpected/duplicate Surface video timestamp");
            try(Image image=value.getOutputImage(index)) {
                check(image!=null && image.getPlanes().length==3 && image.getCropRect().width()==width && image.getCropRect().height()==height,"Missing Surface video decoder pixels");
                double y=planeMean(image,0),u=planeMean(image,1),v=planeMean(image,2),expected=16+219*(64+frame*8)/255.0;
                check(Math.abs(y-expected)<=12 && Math.abs(u-128)<=12 && Math.abs(v-128)<=12,"Decoded Surface video pixels differ");
                pixels.put(new JSONObject().put("frame",frame).put("presentationTimeUs",info.presentationTimeUs).put("expectedLuma",expected).put("y",y).put("u",u).put("v",v));
            }
        });
        check(seen.size()==frames,"Surface video frames lost");
        return new JSONObject().put("encoder",encoded.name).put("decoder",decoder).put("encodedBytes",encoded.bytes).put("packets",encoded.packets.size())
            .put("width",width).put("height",height).put("inputFrames",submitted).put("decodedFrames",seen.size()).put("pixels",pixels)
            .put("outputSurface",surfaceDecode(encoded,stage)).put("encodeMillis",encoded.elapsed).put("inputMode","egl-surface").put("eglVendor",vendor).put("glRenderer",renderer).put("endOfStream",ended);
    }
    private static JSONObject surfaceDecode(Encoded encoded,Stage stage)throws Exception {
        final int width=64,height=64,frames=12;String mime=MediaFormat.MIMETYPE_VIDEO_AVC;
        stage.update("avc-decoder-surface");MediaCodec codec=MediaCodec.createDecoderByType(mime);
        long started=SystemClock.elapsedRealtime();boolean active=false,inputEnd=false,outputEnd=false;int packet=0;
        Set<Integer> seen=new HashSet<>();JSONArray pixels=new JSONArray();
        CodecOutputSurface target=null;
        try {
            target=new CodecOutputSurface(width,height);
            String name=codec.getName();MediaFormat format=MediaFormat.createVideoFormat(mime,width,height);encoded.configure(format);
            codec.configure(format,target.surface(),null,0);codec.start();active=true;
            MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();
            while(!outputEnd) {
                check(SystemClock.elapsedRealtime()-started<45000,"Surface decoder deadline exceeded");
                if(!inputEnd) {
                    int index=codec.dequeueInputBuffer(10000);
                    if(index>=0) {
                        if(packet==encoded.packets.size()){codec.queueInputBuffer(index,0,0,encoded.packets.get(packet-1).pts+100000,MediaCodec.BUFFER_FLAG_END_OF_STREAM);inputEnd=true;}
                        else {
                            Packet value=encoded.packets.get(packet++);ByteBuffer buffer=codec.getInputBuffer(index);
                            check(buffer!=null && buffer.capacity()>=value.data.length,"Surface decoder input too small");
                            buffer.clear();buffer.put(value.data);codec.queueInputBuffer(index,0,value.data.length,value.pts,0);
                        }
                    }
                }
                int index=codec.dequeueOutputBuffer(info,10000);
                if(index>=0) {
                    boolean released=false;
                    try {
                        if(info.size>0) {
                            int frame=(int)(info.presentationTimeUs/100000)-1;
                            check(info.presentationTimeUs%100000==0 && frame>=0 && frame<frames && seen.add(frame),"Unexpected Surface decoder timestamp");
                            MediaFormat output=codec.getOutputFormat(index);
                            check(output.getInteger(MediaFormat.KEY_WIDTH)==width && output.getInteger(MediaFormat.KEY_HEIGHT)==height,"Unexpected Surface decoder dimensions");
                            codec.releaseOutputBuffer(index,true);released=true;pixels.put(target.read(frame,info.presentationTimeUs));
                        }
                        outputEnd=(info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0;
                    }finally{if(!released)codec.releaseOutputBuffer(index,false);}
                }
            }
            check(inputEnd && seen.size()==frames,"Surface decoder frames lost");
            return new JSONObject().put("name",name).put("outputs",seen.size()).put("pixels",pixels).put("outputMode","surface-texture")
                .put("width",width).put("height",height).put("endOfStream",outputEnd).put("eglVendor",target.vendor).put("glRenderer",target.renderer)
                .put("elapsedMillis",SystemClock.elapsedRealtime()-started);
        }finally{try{if(active)codec.stop();}finally{try{codec.release();}finally{if(target!=null)target.close();}}}
    }
    static void run(JSONObject report,Stage stage)throws Exception {
        stage.update("enumerate-codecs");JSONArray names=new JSONArray();
        for(MediaCodecInfo info:new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos())
            names.put(new JSONObject().put("name",info.getName()).put("encoder",info.isEncoder()).put("softwareOnly",info.isSoftwareOnly()).put("types",new JSONArray(Arrays.asList(info.getSupportedTypes()))));
        check(names.length()>0,"No media codecs exposed");report.put("codecs",names);
        report.put("aac",audio(stage));report.put("avc",video(stage));report.put("surfaceAvc",surfaceVideo(stage));stage.update("complete");
    }
}
