// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.bridge;
import java.io.*;
import java.util.Arrays;

public final class HostWireTest {
    static void reject(byte[] bytes,int limit) throws Exception {
        try {HostWire.read(new DataInputStream(new ByteArrayInputStream(bytes)),limit);throw new AssertionError("Malformed frame accepted");}
        catch(IOException expected){ }
    }
    public static void main(String[] args) throws Exception {
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();DataOutputStream out=new DataOutputStream(bytes);
        byte[] payload={0,1,127,(byte)255};HostWire.write(out,payload);
        if(!Arrays.equals(payload,HostWire.read(new DataInputStream(new ByteArrayInputStream(bytes.toByteArray())),4)))
            throw new AssertionError("Binary payload was changed");
        reject(new byte[]{-1,-1,-1,-1},16);
        reject(new byte[]{0,0,0,17},16);
        reject(new byte[]{0,0,0,3,1,2},16);
        reject(new byte[]{1,0,0,1},HostWire.MAX);
        System.out.println("PASS: binary host framing, negative/oversize lengths and truncated packets");
    }
}
