// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.bridge;

import java.io.*;
import java.util.regex.Pattern;

/** A session's guest pixels, independent of the host window presenting them. */
public final class DisplayGeometry {
    private static final int MAGIC=0x504b543a; // Display/input protocol v2, including density.
    public final int width,height,densityDpi;
    public DisplayGeometry(int width,int height,int densityDpi) {
        if(width<1 || height<1 || width>8192 || height>8192 || (long)width*height>16*1024*1024 ||
                densityDpi<1 || densityDpi>1280)throw new IllegalArgumentException("Unsupported guest display geometry");
        this.width=width;this.height=height;this.densityDpi=densityDpi;
    }
    public int[] fit(int availableWidth,int availableHeight) {
        int w=Math.max(1,availableWidth),h=Math.max(1,availableHeight);
        if((long)w*height<=(long)h*width)h=Math.max(1,(int)((long)w*height/width));
        else w=Math.max(1,(int)((long)h*width/height));
        return new int[]{w,h};
    }
    public int maximumFrameBytes(){return Math.toIntExact((long)width*height*4+1024*1024);}
    public void write(DataOutput output) throws IOException {
        output.writeInt(MAGIC);output.writeInt(width);output.writeInt(height);output.writeInt(densityDpi);
    }
    public static DisplayGeometry read(DataInput input) throws IOException {
        if(input.readInt()!=MAGIC)throw new IOException("Unsupported guest display protocol");
        try{return new DisplayGeometry(input.readInt(),input.readInt(),input.readInt());}
        catch(IllegalArgumentException invalid){throw new IOException("Invalid guest display handshake",invalid);}
    }
    private static String property(String source,String key,int value) {
        Pattern pattern=Pattern.compile("(?m)^"+Pattern.quote(key)+"=.*$");
        if(pattern.matcher(source).find())return pattern.matcher(source).replaceAll(key+"="+value);
        return source+(source.endsWith("\n")?"":"\n")+key+"="+value+"\n";
    }
    public String properties(String source) {
        source=property(source,"ro.boot.redroid_width",width);
        source=property(source,"ro.boot.redroid_height",height);
        return property(source,"ro.sf.lcd_density",densityDpi);
    }
    @Override public boolean equals(Object value) {
        if(!(value instanceof DisplayGeometry))return false;
        DisplayGeometry other=(DisplayGeometry)value;
        return width==other.width && height==other.height && densityDpi==other.densityDpi;
    }
    @Override public int hashCode(){return (width*31+height)*31+densityDpi;}
    @Override public String toString(){return width+" × "+height+" · "+densityDpi+" dpi";}
}
