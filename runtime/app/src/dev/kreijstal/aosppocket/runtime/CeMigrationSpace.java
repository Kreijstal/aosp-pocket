// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.IOException;
import java.nio.file.*;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.HashSet;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;

/** Read-only, conservative sizing for an offline encrypted staging copy. */
final class CeMigrationSpace {
    private static final long CHUNK=4096,MAX_CHUNKS=500000,MAX_INDEX=16L*1024*1024;
    private static final long HEADROOM=64L*1024*1024;
    interface Metadata {long bytes(Path path,BasicFileAttributes attributes)throws IOException;}
    static final class Plan {
        final long plaintextBytes,dataChunks,metadataBytes,requiredBytes;
        final int paths;
        Plan(long bytes,long chunks,long metadata,long required,int paths) {
            plaintextBytes=bytes;dataChunks=chunks;metadataBytes=metadata;requiredBytes=required;this.paths=paths;
        }
        void requireAvailable(long available)throws IOException {
            if(available<requiredBytes)throw new IOException("Encrypted storage needs "+((requiredBytes+1048575)/1048576)
                +" MiB free; available "+Math.max(0,available/1048576)+" MiB. Existing data is preserved.");
        }
    }
    private static long add(long a,long b)throws IOException {
        try{return Math.addExact(a,b);}catch(ArithmeticException overflow){throw new IOException("CE migration size overflow",overflow);}
    }
    private static long multiply(long a,long b)throws IOException {
        try{return Math.multiplyExact(a,b);}catch(ArithmeticException overflow){throw new IOException("CE migration size overflow",overflow);}
    }
    private static long rounded(long size,long unit)throws IOException {return multiply(add(size,unit-1)/unit,unit);}
    static Plan inspect(Path[] roots,long unit,Metadata metadata)throws IOException {
        if(unit<512 || unit>1024*1024 || (unit&(unit-1))!=0)throw new IOException("Invalid migration allocation unit");
        final long[] totals={0,0,512};final int[] paths={0},nodes={0},files={0};Set<Object> inodes=new HashSet<>();
        Map<Path,Integer> children=new HashMap<>();
        FileVisitor<Path> visitor=new SimpleFileVisitor<Path>() {
            private void inspect(Path path,BasicFileAttributes attrs)throws IOException {
                if(!attrs.isDirectory() && !attrs.isRegularFile() && !attrs.isSymbolicLink())
                    throw new IOException("Unsupported special file in CE migration");
                if(paths[0]==Integer.MAX_VALUE)throw new IOException("CE migration path count overflow");paths[0]++;
                Integer siblings=children.get(path.getParent());
                if(siblings!=null) {
                    if(siblings>=10000)throw new IOException("CE migration directory exceeds its entry bound");
                    children.put(path.getParent(),siblings+1);
                }
                totals[2]=add(totals[2],6+3L*path.getFileName().toString().length());
                if(attrs.isDirectory())children.put(path,0);
                else {Object key=attrs.fileKey();if(key!=null && !inodes.add(key))return;}
                if(++nodes[0]>9999)throw new IOException("CE migration namespace exceeds its inode bound");
                long extra=metadata.bytes(path,attrs);if(extra<0)throw new IOException("Invalid migration metadata size");
                totals[2]=add(totals[2],add(64,extra));
                if(attrs.isSymbolicLink())totals[2]=add(totals[2],3L*Files.readSymbolicLink(path).toString().length());
                if(!attrs.isRegularFile())return;
                files[0]++;
                long size=attrs.size();if(size<0 || size>(1L<<40))throw new IOException("CE migration file exceeds its size bound");
                totals[0]=add(totals[0],size);totals[1]=add(totals[1],add(size,CHUNK-1)/CHUNK);
            }
            @Override public FileVisitResult preVisitDirectory(Path path,BasicFileAttributes attrs)throws IOException {
                inspect(path,attrs);return FileVisitResult.CONTINUE;
            }
            @Override public FileVisitResult visitFile(Path path,BasicFileAttributes attrs)throws IOException {
                inspect(path,attrs);return FileVisitResult.CONTINUE;
            }
        };
        for(Path root:roots) {
            if(Files.notExists(root,LinkOption.NOFOLLOW_LINKS))continue;
            if(!Files.isDirectory(root,LinkOption.NOFOLLOW_LINKS))throw new IOException("CE source root must be a real directory");
            Files.walkFileTree(root,visitor);
        }
        long metadataChunks=add(totals[2],CHUNK-1)/CHUNK,totalChunks=add(totals[1],metadataChunks);
        // Inode content names are at most "inode-10001"; the namespace name is
        // twelve bytes, object IDs are 32 hex digits, and each block index is a
        // long plus a UTF object ID (42 bytes). Include header/entry slack.
        long index=add(64,add(multiply(files[0]+1L,64),multiply(totalChunks,42)));
        if(totalChunks>MAX_CHUNKS || index>MAX_INDEX)throw new IOException("CE migration exceeds encrypted-store index bounds");
        // Every encrypted 4 KiB chunk has 28 bytes of authentication overhead.
        // Budget a filesystem allocation unit per inode plus directory entries.
        // Migration collects after every checkpoint: only two namespace/index
        // generations need space. Plaintext already occupies its own blocks.
        long perObject=add(rounded(CHUNK+28,unit),unit);
        long objects=add(totals[1],multiply(metadataChunks,2));
        long required=add(HEADROOM,add(multiply(objects,perObject),rounded(multiply(objects,128),unit)));
        required=add(required,add(multiply(rounded(add(index,28),unit),2),multiply(paths[0]+8L,unit)));
        return new Plan(totals[0],totals[1],totals[2],required,paths[0]);
    }
}
