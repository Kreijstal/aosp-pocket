// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.bridge;

import java.io.*;
import java.nio.*;
import java.nio.file.Files;

/** Independent mappings exercise the same ownership transfer used across processes. */
public final class SharedFramesTest {
    private static void require(boolean value,String reason){if(!value)throw new AssertionError(reason);}
    public static void main(String[] args) throws Exception {
        File path=Files.createTempFile("pocket-frames-",".rgba").toFile();
        DisplayGeometry geometry=new DisplayGeometry(3,2,160);
        try {
            try(SharedFrames producer=new SharedFrames(path,geometry,true);
                SharedFrames consumer=new SharedFrames(path,geometry,false)) {
                require(path.length()==48,"Two tightly packed RGBA frames");
                byte[] red={ (byte)255,0,0,(byte)255 };
                for(int sequence=0;sequence<8;sequence++) {
                    int slot=sequence&1;ByteBuffer write=producer.slot(slot);
                    for(int pixel=0;pixel<6;pixel++)write.put(red);
                    write.put(4,(byte)sequence);
                    ByteBuffer read=consumer.slot(slot);
                    require((read.get(0)&255)==255 && read.get(1)==0 && read.get(2)==0,"RGBA channels are preserved");
                    require(read.get(4)==sequence,"Other mapping sees the acknowledged frame");
                    if(sequence>0)require(consumer.slot(1-slot).get(4)==sequence-1,"Writing the back slot leaves the front intact");
                }
                try{producer.slot(2);throw new AssertionError("Invalid slot accepted");}
                catch(IllegalArgumentException expected){}
                require(producer.slot(0).remaining()==24,"Each view is bounded to its own slot");
            }
            try(SharedFrames invalid=new SharedFrames(path,new DisplayGeometry(4,2,160),false)) {
                throw new AssertionError("Mismatched geometry accepted");
            } catch(IOException expected){}
            System.out.println("PASS: coherent RGBA mappings, double-buffer ownership, bounded slots and geometry rejection");
        } finally {Files.delete(path.toPath());}
    }
}
