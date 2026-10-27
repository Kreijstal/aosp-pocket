// SPDX-License-Identifier: GPL-3.0-only
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/** Exercise real OMX buffers, including EOS, without camera/audio hardware. */
public final class MediaCodecProbe {
    // 100 ms mono silence, generated with ffmpeg/libmp3lame at 8 kHz, 8 kb/s,
    // with -write_xing 0 -id3v2_version 0. No external media is used.
    static final String MP3_SILENCE =
        "/+MYxAAAAANIAAAAAExBTUU0LjBVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"+
        "/+MYxDsAAANIAAAAAFVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"+
        "/+MYxHYAAANIAAAAAFVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV"+
        "/+MYxLEAAANIAAAAAFVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV";
    static void require(boolean ok, String detail) {
        if (!ok) throw new AssertionError(detail);
    }
    static byte[] audio(String name, String mime, byte[] input) throws Exception {
        MediaCodec c=MediaCodec.createByCodecName(name);
        try {
            c.configure(MediaFormat.createAudioFormat(mime,8000,1),null,null,0);
            c.start();
            int slot=-1;long inputDeadline=System.nanoTime()+10000000000L;
            while(slot<0 && System.nanoTime()<inputDeadline) slot=c.dequeueInputBuffer(100000);
            require(slot>=0,"audio input unavailable");
            c.getInputBuffer(slot).put(input);
            c.queueInputBuffer(slot,0,input.length,0,MediaCodec.BUFFER_FLAG_END_OF_STREAM);
            java.io.ByteArrayOutputStream out=new java.io.ByteArrayOutputStream();
            MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();
            long deadline=System.nanoTime()+10000000000L;
            boolean eos=false;
            while (!eos && System.nanoTime()<deadline) {
                slot=c.dequeueOutputBuffer(info,100000);
                if(slot<0)continue;
                ByteBuffer b=c.getOutputBuffer(slot);b.position(info.offset);b.limit(info.offset+info.size);
                byte[] bytes=new byte[info.size];b.get(bytes);out.write(bytes);
                eos=(info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0;
                c.releaseOutputBuffer(slot,false);
            }
            require(eos,"audio EOS timeout");c.stop();return out.toByteArray();
        } finally {c.release();}
    }
    static void video() throws Exception {
        MediaCodec enc=MediaCodec.createByCodecName("OMX.google.h264.encoder");
        List<byte[]> packets=new ArrayList<>();MediaFormat encodedFormat=null;
        try {
            MediaFormat f=MediaFormat.createVideoFormat("video/avc",64,64);
            f.setInteger(MediaFormat.KEY_COLOR_FORMAT,MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar);
            f.setInteger(MediaFormat.KEY_BIT_RATE,64000);f.setInteger(MediaFormat.KEY_FRAME_RATE,10);
            f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL,1);
            enc.configure(f,null,null,MediaCodec.CONFIGURE_FLAG_ENCODE);enc.start();
            byte[] pixels=new byte[64*64*3/2];Arrays.fill(pixels,0,4096,(byte)64);
            Arrays.fill(pixels,4096,pixels.length,(byte)128);
            int sent=0;boolean eos=false;MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();
            long deadline=System.nanoTime()+15000000000L;
            while(!eos && System.nanoTime()<deadline) {
                if(sent<4) {
                    int i=enc.dequeueInputBuffer(10000);
                    if(i>=0){enc.getInputBuffer(i).put(pixels);enc.queueInputBuffer(i,0,pixels.length,sent*100000L,
                            sent==3?MediaCodec.BUFFER_FLAG_END_OF_STREAM:0);sent++;}
                }
                int i=enc.dequeueOutputBuffer(info,10000);
                if(i==MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) encodedFormat=enc.getOutputFormat();
                if(i>=0) {
                    if(info.size>0 && (info.flags&MediaCodec.BUFFER_FLAG_CODEC_CONFIG)==0) {
                        byte[] bytes=new byte[info.size];ByteBuffer b=enc.getOutputBuffer(i);
                        b.position(info.offset);b.limit(info.offset+info.size);b.get(bytes);packets.add(bytes);
                    }
                    eos=(info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0;enc.releaseOutputBuffer(i,false);
                }
            }
            require(eos && encodedFormat!=null && !packets.isEmpty(),"AVC encode/EOS timeout");enc.stop();
        } finally {enc.release();}
        MediaCodec dec=MediaCodec.createByCodecName("OMX.google.h264.decoder");int frames=0;
        try {
            dec.configure(encodedFormat,null,null,0);dec.start();
            int sent=0;boolean eos=false;MediaCodec.BufferInfo info=new MediaCodec.BufferInfo();
            long deadline=System.nanoTime()+15000000000L;
            while(!eos && System.nanoTime()<deadline) {
                if(sent<=packets.size()) {
                    int i=dec.dequeueInputBuffer(10000);
                    if(i>=0) {
                        byte[] bytes=sent<packets.size()?packets.get(sent):new byte[0];
                        dec.getInputBuffer(i).put(bytes);dec.queueInputBuffer(i,0,bytes.length,sent*100000L,
                                sent==packets.size()?MediaCodec.BUFFER_FLAG_END_OF_STREAM:0);sent++;
                    }
                }
                int i=dec.dequeueOutputBuffer(info,10000);
                if(i>=0) {
                    if(info.size>0) {
                        int y=dec.getOutputBuffer(i).get(info.offset)&255;
                        require(Math.abs(y-64)<=5,"AVC decoded pixel mismatch: "+y);frames++;
                    }
                    eos=(info.flags&MediaCodec.BUFFER_FLAG_END_OF_STREAM)!=0;dec.releaseOutputBuffer(i,false);
                }
            }
            require(eos && frames==4,"AVC decode frames/EOS mismatch: "+frames);dec.stop();
        } finally {dec.release();}
        System.out.println("MEDIA_AVC_ROUNDTRIP_PASS frames="+frames+" packets="+packets.size());
    }
    public static void main(String[] args) {
        try {
            // Use an ordinary logical app UID, retaining the real host app UID.
            android.system.Os.setgid(19990);android.system.Os.setuid(19990);
            require(android.os.Process.myUid()==19990,"guest credential drop failed");
            long start=System.nanoTime();Set<String> names=new HashSet<>();
            for(MediaCodecInfo c:new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
                names.add(c.getName());System.out.println("MEDIA_CODEC "+c.getName()+" encoder="+c.isEncoder());
            }
            require(names.size()==6,"expected six implemented codecs: "+names);
            System.out.println("MEDIA_ENUMERATION_PASS count="+names.size()+" elapsedMs="+(System.nanoTime()-start)/1000000);
            byte[] pcm=new byte[128];for(int i=0;i<pcm.length;i++)pcm[i]=(byte)i;
            require(Arrays.equals(pcm,audio("OMX.google.raw.decoder","audio/raw",pcm)),"PCM mismatch");
            System.out.println("MEDIA_RAW_PASS bytes="+pcm.length);
            byte[] mulaw=new byte[64];Arrays.fill(mulaw,(byte)255);
            require(Arrays.equals(new byte[128],audio("OMX.google.g711.mlaw.decoder","audio/g711-mlaw",mulaw)),"G711 mismatch");
            byte[] alaw=new byte[64];Arrays.fill(alaw,(byte)0xd5);
            byte[] expectedAlaw=new byte[128];for(int i=0;i<expectedAlaw.length;i+=2)expectedAlaw[i]=8;
            require(Arrays.equals(expectedAlaw,audio("OMX.google.g711.alaw.decoder","audio/g711-alaw",alaw)),"G711 A-law mismatch");
            System.out.println("MEDIA_G711_PASS variants=2 samples=64");
            byte[] silence=audio("OMX.google.mp3.decoder","audio/mpeg",android.util.Base64.decode(MP3_SILENCE,0));
            require(silence.length>=1600 && silence.length%2==0,"MP3 decoded sample count: "+silence.length);
            for(int i=0;i<silence.length;i+=2) {
                int sample=(short)((silence[i]&255)|((silence[i+1]&255)<<8));
                require(Math.abs(sample)<=1,"MP3 silence mismatch: "+sample);
            }
            System.out.println("MEDIA_MP3_PASS pcmBytes="+silence.length);video();
            System.out.println("MEDIA_CODEC_PROBE_PASS uid="+android.os.Process.myUid());System.exit(0);
        } catch(Throwable error){error.printStackTrace();System.exit(1);}
    }
}
