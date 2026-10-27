// SPDX-License-Identifier: GPL-3.0-only
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.os.SystemClock;

/** Play a 440 Hz tone through the guest's stock AudioTrack/AudioFlinger path and check real-time consumption. */
public final class AudioProbe {
    public static void main(String[] args) throws Exception {
        int rate=48000,seconds=args.length>0?Integer.parseInt(args[0]):4;
        AudioTrack track=new AudioTrack.Builder()
            .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
            .setAudioFormat(new AudioFormat.Builder().setSampleRate(rate).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
            .setTransferMode(AudioTrack.MODE_STREAM).build();
        if(track.getState()!=AudioTrack.STATE_INITIALIZED)throw new AssertionError("AudioTrack not initialized");
        System.out.println("AUDIO_PROBE track output_rate="+track.getSampleRate()+" buffer_frames="+track.getBufferSizeInFrames());
        short[] pcm=new short[rate*2/10];// 100 ms stereo
        long start=SystemClock.elapsedRealtime();track.play();
        int frames=0;
        for(int block=0;block<seconds*10;block++) {
            for(int i=0;i<pcm.length/2;i++) {
                short v=(short)(Math.sin(2*Math.PI*440*(frames+i)/rate)*8000);pcm[2*i]=v;pcm[2*i+1]=v;
            }
            int written=track.write(pcm,0,pcm.length);
            if(written!=pcm.length)throw new AssertionError("AudioTrack write returned "+written);
            frames+=pcm.length/2;
        }
        long wrote=SystemClock.elapsedRealtime()-start;
        // Let the mixer drain the track before reading the head position.
        long deadline=SystemClock.elapsedRealtime()+3000;
        while(track.getPlaybackHeadPosition()<frames && SystemClock.elapsedRealtime()<deadline)Thread.sleep(20);
        long elapsed=SystemClock.elapsedRealtime()-start;int head=track.getPlaybackHeadPosition();
        track.stop();track.release();
        System.out.println("AUDIO_PROBE frames="+frames+" head="+head+" write_ms="+wrote+" elapsed_ms="+elapsed);
        // Consumption must follow real time: neither discarded instantly nor stalled.
        boolean realtime=wrote>=seconds*1000-600 && head>=frames*9/10 && elapsed<=seconds*1000+3000;
        System.out.println(realtime?"AUDIO_PROBE_PASS":"AUDIO_PROBE_FAIL");
    }
}
