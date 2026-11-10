// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.content.Context;
import java.io.File;

/** Actual host battery updates for the independent 12L hardware-service test. */
public final class HostHardware implements AutoCloseable {
    private final BootBattery battery;
    public HostHardware(Context context,File root) { battery=new BootBattery(context,root); }
    @Override public void close() { battery.close(); }
}
