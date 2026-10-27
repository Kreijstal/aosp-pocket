// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.*;
import java.nio.file.*;
import java.nio.file.attribute.PosixFilePermission;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.*;

/** Validates all immutable system files before replacing any installed file. */
final class ImageArchive {
    static final long LIMIT=2L*1024*1024*1024;
    static final class Entry {
        final long size,crc;final String sha;final int mode;
        Entry(long size,long crc,String sha,int mode)throws IOException {
            if(size<0 || size>LIMIT || crc<0 || crc>0xffffffffL || !sha.matches("[0-9a-f]{64}") || mode<0 || mode>0777)
                throw new IOException("Invalid image file metadata");
            this.size=size;this.crc=crc;this.sha=sha;this.mode=mode;
        }
    }
    static void path(String name)throws IOException {
        if(!(name.startsWith("system/") || name.startsWith("vendor/") || name.startsWith("apex/")) || name.indexOf('\\')>=0 || name.indexOf('\0')>=0)
            throw new IOException("Invalid image path");
        for(String part:name.split("/",-1))if(part.isEmpty() || part.equals(".") || part.equals(".."))throw new IOException("Invalid image path");
    }
    static File target(File root,String name)throws IOException {
        if(Files.isSymbolicLink(root.toPath()))throw new IOException("Image root is a symlink");
        path(name);File result=new File(root,name);
        if(!result.getCanonicalPath().startsWith(root.getCanonicalPath()+"/"))throw new IOException("Image path escapes root");
        for(File parent=result;parent!=null && !parent.equals(root);parent=parent.getParentFile())
            if(Files.isSymbolicLink(parent.toPath()))throw new IOException("Image path contains a symlink");
        return result;
    }
    static void atomicText(File file,String value)throws IOException {
        File temporary=new File(file.getPath()+".new");
        if(Files.isSymbolicLink(file.toPath()) || Files.isSymbolicLink(temporary.toPath()))throw new IOException("Invalid image marker path");
        try(FileOutputStream out=new FileOutputStream(temporary)){out.write(value.getBytes("UTF-8"));out.getFD().sync();}
        ImageDownload.move(temporary,file);
    }
    static long validate(ZipFile zip,File root,Map<String,Entry> files,String manifestSha)throws IOException {
        Set<String> seen=new HashSet<>();long total=0;
        for(Enumeration<? extends ZipEntry> all=zip.entries();all.hasMoreElements();) {
            ZipEntry member=all.nextElement();String name=member.getName();
            if(!seen.add(name) || member.isDirectory())throw new IOException("Duplicate or unexpected image entry");
            if(name.equals("pocket-image.json")) {
                if(member.getSize()<0 || member.getSize()>8*1024*1024)throw new IOException("Invalid image manifest size");
                try(InputStream in=zip.getInputStream(member)) {
                    if(!ImageDownload.hash(in,8*1024*1024).equals(manifestSha))throw new IOException("Image manifest checksum differs");
                }
                continue;
            }
            Entry spec=files.get(name);target(root,name);
            if(spec==null || member.getSize()!=spec.size || member.getCrc()!=spec.crc || spec.size>LIMIT-total)
                throw new IOException("Image entry differs from catalog: "+name);
            total+=spec.size;
            try(InputStream in=zip.getInputStream(member)) {
                if(!ImageDownload.hash(in,spec.size).equals(spec.sha))throw new IOException("Image file checksum differs: "+name);
            }
        }
        if(!seen.contains("pocket-image.json") || seen.size()!=files.size()+1 || !seen.containsAll(files.keySet()))
            throw new IOException("Image is missing required files");
        return total;
    }
    static void install(File archive,File root,Map<String,Entry> files,String manifestSha,String imageSha,ImageDownload.Progress progress)throws IOException {
        if(!root.isDirectory() && !root.mkdirs())throw new IOException("Cannot create guest image directory");
        try(ZipFile zip=new ZipFile(archive)) {
            long total=validate(zip,root,files,manifestSha),done=0;
            atomicText(new File(root,".pocket-image-installing"),imageSha+"\n");
            progress.update(0,total);
            for(Map.Entry<String,Entry> item:files.entrySet()) {
                ImageDownload.cancelled();File file=target(root,item.getKey()),temporary=target(root,item.getKey()+".pocket-new");Entry spec=item.getValue();
                if(!file.getParentFile().isDirectory() && !file.getParentFile().mkdirs())throw new IOException("Cannot create image directory");
                MessageDigest hash=ImageDownload.digest();long written=0;
                try(InputStream in=zip.getInputStream(zip.getEntry(item.getKey()));FileOutputStream out=new FileOutputStream(temporary)) {
                    byte[] buffer=new byte[65536];
                    for(int n;(n=in.read(buffer))!=-1;) {
                        ImageDownload.cancelled();if(n>spec.size-written)throw new IOException("Image file exceeds declared size");
                        out.write(buffer,0,n);hash.update(buffer,0,n);written+=n;
                    }
                    out.getFD().sync();
                }
                if(written!=spec.size || !ImageDownload.hex(hash.digest()).equals(spec.sha))throw new IOException("Image changed during installation");
                Set<PosixFilePermission> permissions=EnumSet.noneOf(PosixFilePermission.class);
                PosixFilePermission[] bits=PosixFilePermission.values();
                for(int bit=0;bit<9;bit++)if((spec.mode&(0400>>bit))!=0)permissions.add(bits[bit]);
                Files.setPosixFilePermissions(temporary.toPath(),permissions);
                ImageDownload.move(temporary,file);done+=written;progress.update(done,total);
            }
        }
    }
}
