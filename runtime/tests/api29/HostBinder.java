// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.content.Context;
import android.content.IntentFilter;
import java.io.File;

/** Reuses Pocket's package-scoped host handoff for the separate API 29 fixture. */
public final class HostBinder {
    private final Context context;
    private final GuestBinder receiver=new GuestBinder();
    private final GuestPower power;

    public HostBinder(Context context,File root)throws Exception {
        this.context=context;
        GuestBinder.preparePlatform(context,root);
        power=new GuestPower(context,root);
        try{context.registerReceiver(receiver,new IntentFilter(GuestBinder.ACTION));}
        catch(RuntimeException error){power.close();throw error;}
    }
    public void close(){context.unregisterReceiver(receiver);receiver.reset();power.close();}
}
