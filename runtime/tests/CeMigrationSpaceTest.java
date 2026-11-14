// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.*;
import java.nio.file.*;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.Arrays;

/** Real filesystem coverage; never creates an encrypted vault or credentials. */
public final class CeMigrationSpaceTest {
    private static void check(boolean value)throws IOException {if(!value)throw new IOException("Migration space assertion failed");}
    private interface Attempt {void run()throws Exception;}
    private static void rejected(Attempt action)throws Exception {
        try{action.run();throw new AssertionError("Expected preflight rejection");}catch(IOException expected){}
    }
    private static void remove(Path root)throws IOException {
        Files.walkFileTree(root,new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult visitFile(Path file,BasicFileAttributes attrs)throws IOException {Files.delete(file);return FileVisitResult.CONTINUE;}
            @Override public FileVisitResult postVisitDirectory(Path dir,IOException error)throws IOException {if(error!=null)throw error;Files.delete(dir);return FileVisitResult.CONTINUE;}
        });
    }
    public static void main(String[] args)throws Exception {
        Path work=Files.createTempDirectory("pocket-ce-space-");
        try {
            Path one=Files.createDirectory(work.resolve("one")),two=Files.createDirectory(work.resolve("two"));
            byte[] bytes=new byte[4097];Arrays.fill(bytes,(byte)17);
            Path original=Files.write(one.resolve("original"),bytes);
            Files.createLink(two.resolve("hardlink"),original);
            Files.write(two.resolve("small"),new byte[]{1,2,3});Files.createFile(one.resolve("empty"));
            Path outside=work.resolve("outside");
            try(RandomAccessFile file=new RandomAccessFile(outside.toFile(),"rw")){file.setLength(3L<<30);}
            Files.createSymbolicLink(one.resolve("outside-link"),outside);
            Files.createSymbolicLink(one.resolve("directory-link"),two);
            Path[] roots={one,two,work.resolve("absent")};
            CeMigrationSpace.Metadata attributes=(path,stat)->0;
            CeMigrationSpace.Plan plan=CeMigrationSpace.inspect(roots,4096,attributes);
            check(plan.plaintextBytes==4100 && plan.dataChunks==3 && plan.paths==8);
            check(CeMigrationSpace.inspect(roots,16384,attributes).requiredBytes>plan.requiredBytes);
            check(CeMigrationSpace.inspect(roots,4096,(path,stat)->8192).requiredBytes>plan.requiredBytes);
            plan.requireAvailable(plan.requiredBytes);
            for(long available:new long[]{-1,0,plan.requiredBytes-1})rejected(()->plan.requireAvailable(available));
            check(Arrays.equals(Files.readAllBytes(original),bytes) && Files.isSameFile(original,two.resolve("hardlink")));
            check(Files.isSymbolicLink(one.resolve("outside-link")) && !Files.exists(work.resolve(".pocket-ce-v1")));
            for(long unit:new long[]{0,511,513,2L*1024*1024})rejected(()->CeMigrationSpace.inspect(roots,unit,attributes));
            Path rootLink=Files.createSymbolicLink(work.resolve("root-link"),one);
            rejected(()->CeMigrationSpace.inspect(new Path[]{rootLink},4096,attributes));
            rejected(()->CeMigrationSpace.inspect(new Path[]{original},4096,attributes));
            rejected(()->CeMigrationSpace.inspect(roots,4096,(path,stat)->-1));
            rejected(()->CeMigrationSpace.inspect(roots,4096,(path,stat)->Long.MAX_VALUE));
            Files.move(outside,two.resolve("too-large"));
            rejected(()->CeMigrationSpace.inspect(roots,4096,attributes));Files.delete(two.resolve("too-large"));
            Path fifo=one.resolve("fifo");check(new ProcessBuilder("mkfifo",fifo.toString()).start().waitFor()==0);
            rejected(()->CeMigrationSpace.inspect(roots,4096,attributes));Files.delete(fifo);
            System.out.println("CE_MIGRATION_SPACE_PASS nofollow=1 hardlinks=1 allocation_units=2 store_bounds=1 insufficient_space_no_writes=1");
        }finally{remove(work);}
    }
}
