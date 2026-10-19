// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.content.*;
import android.os.*;
import java.io.*;
import java.nio.file.*;

/** Relay the host Android battery reading; no battery values are invented. */
final class BootBattery extends BroadcastReceiver implements AutoCloseable {
    private final Context context;
    private final File root;
    private final HandlerThread thread=new HandlerThread("PocketBatteryRelay");
    private final Handler handler;
    private Intent latest;
    private volatile boolean closed;
    BootBattery(Context owner,File directory) {
        context=owner.getApplicationContext();root=directory;thread.start();handler=new Handler(thread.getLooper());
        IntentFilter filter=new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
        Intent sticky=Build.VERSION.SDK_INT>=33 ? context.registerReceiver(this,filter,null,handler,Context.RECEIVER_NOT_EXPORTED)
                                               : context.registerReceiver(this,filter,null,handler);
        handler.post(()->{if (sticky!=null) latest=sticky;});
        handler.post(new Runnable(){public void run(){if (closed) return;write();handler.postDelayed(this,2000);}});
    }
    @Override public void onReceive(Context c,Intent intent) { latest=intent;write(); }
    private void write() {
        if (closed || latest==null) return;
        try {
            BatteryManager battery=(BatteryManager)context.getSystemService(Context.BATTERY_SERVICE);
            long[] values={SystemClock.elapsedRealtime(),latest.getBooleanExtra(BatteryManager.EXTRA_PRESENT,false)?1:0,
                latest.getIntExtra(BatteryManager.EXTRA_PLUGGED,0),latest.getIntExtra(BatteryManager.EXTRA_STATUS,1),
                latest.getIntExtra(BatteryManager.EXTRA_HEALTH,1),latest.getIntExtra(BatteryManager.EXTRA_LEVEL,-1),
                latest.getIntExtra(BatteryManager.EXTRA_SCALE,0),latest.getIntExtra(BatteryManager.EXTRA_VOLTAGE,0),
                latest.getIntExtra(BatteryManager.EXTRA_TEMPERATURE,0),
                battery.getIntProperty(BatteryManager.BATTERY_PROPERTY_CHARGE_COUNTER),
                battery.getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW),
                battery.getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_AVERAGE),
                battery.getLongProperty(BatteryManager.BATTERY_PROPERTY_ENERGY_COUNTER)};
            StringBuilder text=new StringBuilder("POCKET_BATTERY_V1\n");
            for (long value:values) text.append(value).append(' ');
            String tech=latest.getStringExtra(BatteryManager.EXTRA_TECHNOLOGY);
            text.append('\n').append(tech==null?"":tech.replace('\n',' ').replace('\r',' ')).append('\n');
            File temp=new File(root,"pocket-host-battery.tmp"), target=new File(root,"pocket-host-battery");
            Files.write(temp.toPath(),text.toString().getBytes("UTF-8"));
            Files.move(temp.toPath(),target.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
        } catch(Exception error) { android.util.Log.w("PocketBatteryRelay","Host battery reading unavailable",error); }
    }
    @Override public void close() {
        closed=true;handler.removeCallbacksAndMessages(null);context.unregisterReceiver(this);thread.quitSafely();
    }
}
