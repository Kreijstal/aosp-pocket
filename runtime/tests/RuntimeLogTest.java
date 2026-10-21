// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import java.io.*;
import java.nio.file.*;
import java.util.Arrays;

public final class RuntimeLogTest {
    public static void main(String[] args) throws Exception {
        Path dir=Files.createTempDirectory("pocket-log-test-");
        Path log=dir.resolve("console.log"),previous=dir.resolve("console.log.previous");
        byte[] data=new byte[4096];for(int i=0;i<data.length;i++)data[i]=(byte)i;
        try {
            Files.write(log,data);
            try(FileOutputStream writer=new FileOutputStream(log.toFile(),true)) {
                RuntimeLog.compact(log.toFile(),1024,128);
                if(Files.size(log)!=0 || !Arrays.equals(Files.readAllBytes(previous),Arrays.copyOfRange(data,3968,4096)))
                    throw new AssertionError("Bounded tail was not preserved");
                writer.write(new byte[]{11,22,33});writer.flush();
                if(!Arrays.equals(Files.readAllBytes(log),new byte[]{11,22,33}))
                    throw new AssertionError("Open append writer left a hole after trimming");
            }
            RuntimeLog.compact(log.toFile(),1024,128);
            if(Files.size(log)!=3)throw new AssertionError("Small log changed");
            Files.write(log,data);RuntimeLog.compact(log.toFile(),1024,64);
            if(Files.size(previous)!=64)throw new AssertionError("Previous tail was not replaced");
            Files.delete(log);Files.createSymbolicLink(log,previous);
            RuntimeLog.compact(log.toFile(),8,4);
            if(Files.size(previous)!=64)throw new AssertionError("Symlink target changed");
            System.out.println("PASS: bounded tail, append after trim, rotation replacement, small log and symlink preservation");
        } finally {
            Files.deleteIfExists(log);Files.deleteIfExists(previous);Files.delete(dir);
        }
    }
}
