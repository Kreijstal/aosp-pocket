// SPDX-License-Identifier: GPL-3.0-only
import android.os.SystemClock;
import java.nio.file.Files;
import java.nio.file.Paths;
import org.json.JSONArray;
import org.json.JSONObject;

/** Check real ART compilation independently of a guest restart. */
public final class GuestJitProbe {
    private static volatile int result;
    private static int work(int value) {
        for(int i=0;i<10000;i++)value=(value*1664525+1013904223)^(value>>>11);
        return value;
    }
    public static void main(String[] args) throws Exception {
        JSONArray rounds=new JSONArray();int value=(int)SystemClock.elapsedRealtime();
        for(int round=0;round<4;round++) {
            long start=SystemClock.elapsedRealtimeNanos();
            for(int i=0;i<500;i++)value=work(value+i);
            result=value;rounds.put((SystemClock.elapsedRealtimeNanos()-start)/1000000.0);
            Thread.sleep(100);
        }
        String maps=new String(Files.readAllBytes(Paths.get("/proc/self/maps")),"UTF-8");
        System.out.println("GUEST_JIT_RESULT "+new JSONObject().put("roundMs",rounds)
                .put("jitCodeCache",maps.contains("jit-code-cache")).put("compilerLoaded",maps.contains("libart-compiler")));
        System.exit(0);
    }
}
