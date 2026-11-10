// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.api32test;
import java.io.File;
/** The fixture uses the same bounded guest-log collector as Manager. */
final class NativeLog extends dev.kreijstal.aosppocket.runtime.GuestLog32 {
    NativeLog(File root)throws Exception {super(root);}
}
