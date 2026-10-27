// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.*;
import java.net.*;
import java.nio.file.*;
import java.security.*;

/** Private, resumable download of an APK-pinned immutable image. */
final class ImageDownload {
    interface Progress { void update(long done,long total); }
    static void checkUrl(URL url)throws IOException {
        if(!url.getProtocol().equals("https") && !(url.getProtocol().equals("http") && url.getHost().equals("127.0.0.1")))
            throw new IOException("Image download requires HTTPS");
        if(url.getUserInfo()!=null || url.getRef()!=null)throw new IOException("Invalid image download URL");
    }
    static String sha256(File file)throws IOException {
        try(InputStream in=new FileInputStream(file)){return hash(in,Long.MAX_VALUE);}
    }
    static String hash(InputStream in,long limit)throws IOException {
        MessageDigest digest=digest();byte[] buffer=new byte[65536];long count=0;
        for(int n;(n=in.read(buffer))!=-1;) {
            cancelled();count+=n;if(count>limit)throw new IOException("Image exceeds declared size");digest.update(buffer,0,n);
        }
        return hex(digest.digest());
    }
    static MessageDigest digest() {
        try{return MessageDigest.getInstance("SHA-256");}
        catch(NoSuchAlgorithmException unavailable){throw new AssertionError(unavailable);}
    }
    static String hex(byte[] bytes) {
        StringBuilder text=new StringBuilder();for(byte b:bytes)text.append(String.format(java.util.Locale.ROOT,"%02x",b&255));return text.toString();
    }
    static void cancelled()throws IOException {
        if(Thread.currentThread().isInterrupted())throw new InterruptedIOException("Image setup cancelled");
    }
    static void move(File from,File to)throws IOException {
        Files.move(from.toPath(),to.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
    }
    static File fetch(URL url,File directory,long bytes,String sha,Progress progress)throws IOException {
        checkUrl(url);
        if(bytes<=0 || bytes>2L*1024*1024*1024 || !sha.matches("[0-9a-f]{64}"))throw new IOException("Invalid pinned image download");
        if(Files.isSymbolicLink(directory.toPath()))throw new IOException("Image cache is a symlink");
        if(!directory.isDirectory() && !directory.mkdirs())throw new IOException("Cannot create image cache");
        File ready=new File(directory,sha+".zip"),part=new File(directory,sha+".part");
        if(Files.isSymbolicLink(ready.toPath()) || Files.isSymbolicLink(part.toPath()))throw new IOException("Invalid image cache path");
        if(ready.isFile()) {
            if(ready.length()==bytes && sha256(ready).equals(sha))return ready;
            Files.delete(ready.toPath());
        }
        long offset=part.isFile()?part.length():0;
        if(offset>bytes){Files.delete(part.toPath());offset=0;}
        if(offset<bytes) {
            HttpURLConnection connection=null;
            try {
                URL next=url;
                for(int redirects=0;;redirects++) {
                    cancelled();checkUrl(next);connection=(HttpURLConnection)next.openConnection();
                    connection.setInstanceFollowRedirects(false);connection.setConnectTimeout(15000);connection.setReadTimeout(20000);
                    connection.setRequestProperty("Accept-Encoding","identity");
                    if(offset>0)connection.setRequestProperty("Range","bytes="+offset+"-");
                    int status=connection.getResponseCode();
                    if(status==301 || status==302 || status==303 || status==307 || status==308) {
                        String location=connection.getHeaderField("Location");
                        if(redirects>=5 || location==null)throw new IOException("Invalid image download redirect");
                        URL redirected=new URL(next,location);checkUrl(redirected);
                        if(next.getProtocol().equals("https") && !redirected.getProtocol().equals("https"))throw new IOException("Insecure image download redirect");
                        connection.disconnect();connection=null;next=redirected;continue;
                    }
                    if(status==200)offset=0;
                    else if(status==206) {
                        String range="bytes "+offset+"-"+(bytes-1)+"/"+bytes;
                        if(!range.equals(connection.getHeaderField("Content-Range")))throw new IOException("Invalid image download range");
                    } else throw new IOException("Image download failed (HTTP "+status+")");
                    long length=connection.getContentLengthLong();
                    if(length>=0 && length!=bytes-offset)throw new IOException("Image download size differs from catalog");
                    long received=offset;progress.update(received,bytes);
                    try(InputStream in=connection.getInputStream();FileOutputStream out=new FileOutputStream(part,offset>0)) {
                        byte[] buffer=new byte[65536];
                        for(int n;(n=in.read(buffer))!=-1;) {
                            cancelled();if(n>bytes-received)throw new IOException("Image download exceeds declared size");
                            out.write(buffer,0,n);received+=n;progress.update(received,bytes);
                        }
                        out.getFD().sync();
                    }
                    if(received!=bytes)throw new IOException("Image download interrupted; tap Start to resume");
                    break;
                }
            } finally {if(connection!=null)connection.disconnect();}
        }
        if(part.length()!=bytes || !sha256(part).equals(sha)) {
            Files.deleteIfExists(part.toPath());throw new IOException("Image checksum verification failed");
        }
        move(part,ready);return ready;
    }
}
