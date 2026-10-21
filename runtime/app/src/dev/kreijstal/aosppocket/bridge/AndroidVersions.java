// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.bridge;

/** Android release names for package compatibility messages. */
public final class AndroidVersions {
    private AndroidVersions() { }
    public static String name(int sdk) {
        switch(sdk) {
            case 23:return "Android 6";
            case 24:return "Android 7";
            case 25:return "Android 7.1";
            case 26:return "Android 8";
            case 27:return "Android 8.1";
            case 28:return "Android 9";
            case 29:return "Android 10";
            case 30:return "Android 11";
            case 31:return "Android 12";
            case 32:return "Android 12L";
            case 33:return "Android 13";
            case 34:return "Android 14";
            case 35:return "Android 15";
            case 36:return "Android 16";
            default:return "Android API "+sdk;
        }
    }
    public static String requirement(int minimum,int guest) {
        return "Requires "+name(minimum)+" or later. This guest runs "+name(guest)+".";
    }
}
