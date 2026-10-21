// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.*;
import java.nio.file.*;

/** Periodically trims an O_APPEND console, retaining one bounded recent tail. */
final class RuntimeLog implements AutoCloseable {
    private final Thread worker;
    private volatile boolean closed;
    RuntimeLog(File file) {
        worker=new Thread(()->{
            while(!closed) {
                try {compact(file,16L*1024*1024,2*1024*1024);}
                catch(IOException ignored) { /* Logging must not prevent guest boot. */ }
                try {Thread.sleep(2000);}
                catch(InterruptedException stop) {return;}
            }
        },"PocketLogLimit");
        worker.setDaemon(true);worker.start();
    }
    static void compact(File file,long limit,int keep) throws IOException {
        if(limit<1 || keep<1 || keep>limit)throw new IllegalArgumentException("Invalid log limits");
        Path path=file.toPath();
        if(Files.isSymbolicLink(path) || !file.isFile() || file.length()<=limit)return;
        File previous=new File(file.getParentFile(),file.getName()+".previous");
        Path staged=Files.createTempFile(file.getParentFile().toPath(),"pocket-log-",".tmp");
        try(RandomAccessFile source=new RandomAccessFile(file,"rw")) {
            long size=source.length();
            if(size<=limit)return;
            int count=(int)Math.min(size,keep);
            source.seek(size-count);
            byte[] tail=new byte[count];source.readFully(tail);
            Files.write(staged,tail);
            Files.move(staged,previous.toPath(),StandardCopyOption.REPLACE_EXISTING);
            // Writers use O_APPEND, so their next write starts at the new EOF
            // rather than recreating a sparse file at an old descriptor offset.
            source.setLength(0);
        } finally {Files.deleteIfExists(staged);}
    }
    @Override public void close(){closed=true;worker.interrupt();}
}
