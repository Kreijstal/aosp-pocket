// SPDX-License-Identifier: GPL-3.0-only
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.VibrationEffect;
import java.lang.reflect.Method;

/**
 * Drive the guest's stock VibratorService: timed and cancelled vibrations, explicit amplitudes,
 * an amplitude waveform and each predefined effect. With "click INTENSITY", set the guest's haptic
 * feedback intensity (1 low .. 3 high), perform one CLICK and restore the setting, so the caller can
 * check how intensity maps to effect strength.
 */
public final class VibratorProbe {
    static final String[] EFFECTS={"CLICK","DOUBLE_CLICK","TICK","THUD","POP","HEAVY_CLICK"};
    public static void main(String[] args) throws Exception {
        IBinder binder=(IBinder)Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,"vibrator");
        if(binder==null)throw new AssertionError("Guest vibrator service missing");
        Object service=Class.forName("android.os.IVibratorService$Stub").getMethod("asInterface",IBinder.class).invoke(null,binder);
        Class<?> api=service.getClass();
        boolean present=(Boolean)api.getMethod("hasVibrator").invoke(service);
        System.out.println("VIBRATOR_PROBE has_vibrator="+present);
        if(!present)return;
        Method vibrate=api.getMethod("vibrate",int.class,String.class,VibrationEffect.class,int.class,IBinder.class);
        // Prebaked effects without the service's fallback waveform, so only a host effect can play them.
        Method prebaked=VibrationEffect.class.getMethod("get",int.class,boolean.class);
        IBinder token=new Binder();
        // uid 0 maps to AppOps' "root" package.
        if(args.length>1 && args[0].equals("click")) {
            String key="haptic_feedback_intensity",saved=setting("GET_system",key,null);
            setting("PUT_system",key,args[1]);
            try {
                Thread.sleep(500);// VibratorService picks the change up through its settings observer.
                vibrate.invoke(service,0,"root",prebaked.invoke(null,0,false),0,token);
                Thread.sleep(500);
                System.out.println("VIBRATOR_PROBE effect=CLICK haptic_feedback_intensity="+args[1]);
            } finally {
                if(saved!=null)setting("PUT_system",key,saved);else setting("DELETE_system",key,null);
            }
            System.out.println("VIBRATOR_PROBE_DONE");
            return;
        }
        System.out.println("VIBRATOR_PROBE amplitude_control="+api.getMethod("hasAmplitudeControl").invoke(service));
        vibrate.invoke(service,0,"root",VibrationEffect.createOneShot(400,VibrationEffect.DEFAULT_AMPLITUDE),0,token);
        System.out.println("VIBRATOR_PROBE vibrate_ms=400");
        Thread.sleep(800);
        vibrate.invoke(service,0,"root",VibrationEffect.createOneShot(3000,VibrationEffect.DEFAULT_AMPLITUDE),0,token);
        Thread.sleep(300);
        api.getMethod("cancelVibrate",IBinder.class).invoke(service,token);
        System.out.println("VIBRATOR_PROBE vibrate_ms=3000 cancelled_after_ms=300");
        Thread.sleep(300);
        for(int amplitude:new int[]{60,255}) {
            vibrate.invoke(service,0,"root",VibrationEffect.createOneShot(300,amplitude),0,token);
            System.out.println("VIBRATOR_PROBE vibrate_ms=300 amplitude="+amplitude);
            Thread.sleep(600);
        }
        // One on() for the whole waveform, then setAmplitude() at each step.
        vibrate.invoke(service,0,"root",VibrationEffect.createWaveform(new long[]{300,300,300},new int[]{60,160,255},-1),0,token);
        System.out.println("VIBRATOR_PROBE waveform_ms=900 amplitudes=60,160,255");
        Thread.sleep(1300);
        for(int id=0;id<EFFECTS.length;id++) {
            vibrate.invoke(service,0,"root",prebaked.invoke(null,id,false),0,token);
            System.out.println("VIBRATOR_PROBE effect="+EFFECTS[id]);
            Thread.sleep(500);
        }
        System.out.println("VIBRATOR_PROBE_DONE");
    }
    /** Settings provider call, as the settings command makes it, for user 0. */
    static String setting(String method,String key,String value) throws Exception {
        Object manager=Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
        IBinder token=new Binder();
        Object holder=manager.getClass().getMethod("getContentProviderExternal",String.class,int.class,IBinder.class)
            .invoke(manager,"settings",0,token);
        try {
            Object provider=holder.getClass().getField("provider").get(holder);
            Bundle extras=new Bundle();extras.putInt("_user",0);
            if(value!=null)extras.putString("value",value);
            Bundle reply=(Bundle)provider.getClass().getMethod("call",String.class,String.class,String.class,Bundle.class)
                .invoke(provider,"root",method,key,extras);
            return reply==null?null:reply.getString("value");
        } finally {
            manager.getClass().getMethod("removeContentProviderExternal",String.class,IBinder.class).invoke(manager,"settings",token);
        }
    }
}
