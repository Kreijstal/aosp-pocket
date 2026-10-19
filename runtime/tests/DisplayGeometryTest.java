// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.bridge;

import java.io.*;
import java.util.Arrays;

public final class DisplayGeometryTest {
    private static void require(boolean condition,String message){if(!condition)throw new AssertionError(message);}
    private static void reject(int w,int h,int dpi) {
        try{new DisplayGeometry(w,h,dpi);throw new AssertionError("Invalid geometry accepted");}
        catch(IllegalArgumentException expected){}
    }
    public static void main(String[] args) throws Exception {
        DisplayGeometry phone=new DisplayGeometry(1080,2400,420);
        require(Arrays.equals(phone.fit(320,800),new int[]{320,711}),"Embedded portrait fit");
        require(Arrays.equals(phone.fit(600,300),new int[]{135,300}),"Height-limited window fit");
        require(Arrays.equals(new DisplayGeometry(2400,1080,420).fit(320,500),new int[]{320,144}),"Landscape fit");
        require(Arrays.equals(phone.fit(0,0),new int[]{1,1}),"Empty surface transition");
        require(Arrays.equals(phone.fit(2160,4800),new int[]{2160,4800}),"Larger window preserves aspect ratio");
        String properties=phone.properties("ro.build.version.sdk=28\nro.boot.redroid_width=480\nro.boot.redroid_height=800\nro.sf.lcd_density=320\n");
        require(properties.equals("ro.build.version.sdk=28\nro.boot.redroid_width=1080\nro.boot.redroid_height=2400\nro.sf.lcd_density=420\n"),"Boot property projection");
        require(phone.properties(properties).equals(properties),"Repeated boot is stable");
        require(phone.properties("ro.build.version.sdk=28").contains("\nro.sf.lcd_density=420\n"),"Missing properties are inserted");
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();phone.write(new DataOutputStream(bytes));
        require(phone.equals(DisplayGeometry.read(new DataInputStream(new ByteArrayInputStream(bytes.toByteArray())))),"Handshake round trip");
        byte[] invalid=bytes.toByteArray();invalid[0]=0;
        try{DisplayGeometry.read(new DataInputStream(new ByteArrayInputStream(invalid)));throw new AssertionError("Old/bad protocol accepted");}
        catch(IOException expected){}
        byte[] oldProtocol=bytes.toByteArray();oldProtocol[3]=0x3b;
        try{DisplayGeometry.read(new DataInputStream(new ByteArrayInputStream(oldProtocol)));throw new AssertionError("PNG protocol accepted by the shared-frame client");}
        catch(IOException expected){}
        require(new DisplayGeometry(2160,3840,640).maximumFrameBytes()>8*1024*1024,"Native 4K frame limit");
        reject(0,2400,420);reject(1080,-1,420);reject(1080,2400,0);reject(8192,8192,420);
        System.out.println("PASS: host geometry, aspect-preserving cards/windows, boot properties, versioned handshake and frame bounds");
    }
}
