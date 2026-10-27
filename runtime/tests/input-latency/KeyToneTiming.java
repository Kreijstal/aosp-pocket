// SPDX-License-Identifier: GPL-3.0-only
import android.media.AudioManager;
import android.media.ToneGenerator;
import android.os.SystemClock;

/** Isolate the synchronous DTMF audio calls used by a guest dial pad. */
public final class KeyToneTiming {
    public static void main(String[] args) throws Exception {
        long began=SystemClock.elapsedRealtime();
        ToneGenerator tone=new ToneGenerator(AudioManager.STREAM_DTMF,0);
        System.out.println("KEY_TONE_CREATE_MS "+(SystemClock.elapsedRealtime()-began));
        try {
            for(int i=0;i<3;i++) {
                began=SystemClock.elapsedRealtime();
                boolean accepted=tone.startTone(ToneGenerator.TONE_DTMF_1,80);
                System.out.println("KEY_TONE_START_MS "+(SystemClock.elapsedRealtime()-began)+" ACCEPTED "+accepted);
                Thread.sleep(150);
            }
        } finally {tone.release();}
        System.out.println("KEY_TONE_TIMING_DONE");System.exit(0);
    }
}
