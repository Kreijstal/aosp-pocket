// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import com.sun.net.httpserver.*;
import java.io.*;
import java.net.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicLong;
import java.util.zip.*;

/** Real HTTP range behavior and archive recovery/path/data-preservation checks. */
public final class ImageDownloadTest {
    private static int assertions;
    private static void check(boolean condition,String message) {
        assertions++;if(!condition)throw new AssertionError(message);
    }
    interface Checked {void run()throws Exception;}
    private static void rejected(Checked work)throws Exception {
        try{work.run();throw new AssertionError("Expected rejection");}catch(IOException expected){assertions++;}
    }
    private static String sha(byte[] data)throws Exception{return ImageDownload.hex(ImageDownload.digest().digest(data));}
    private static final ImageDownload.Progress PROGRESS=(done,total)->{};
    private static final byte[] MANIFEST="{}\n".getBytes(java.nio.charset.StandardCharsets.UTF_8);
    private static File zip(Path dir,Map<String,byte[]> files)throws Exception {
        File output=dir.resolve("image-"+UUID.randomUUID()+".zip").toFile();
        try(ZipOutputStream zip=new ZipOutputStream(new FileOutputStream(output))) {
            zip.putNextEntry(new ZipEntry("pocket-image.json"));zip.write(MANIFEST);zip.closeEntry();
            for(Map.Entry<String,byte[]> entry:files.entrySet()){zip.putNextEntry(new ZipEntry(entry.getKey()));zip.write(entry.getValue());zip.closeEntry();}
        }
        return output;
    }
    private static Map<String,ImageArchive.Entry> specs(Map<String,byte[]> data)throws Exception {
        Map<String,ImageArchive.Entry> result=new TreeMap<>();
        for(Map.Entry<String,byte[]> entry:data.entrySet()) {
            CRC32 crc=new CRC32();crc.update(entry.getValue());
            result.put(entry.getKey(),new ImageArchive.Entry(entry.getValue().length,crc.getValue(),sha(entry.getValue()),0755));
        }
        return result;
    }
    public static void main(String[] args)throws Exception {
        Path temp=Files.createTempDirectory("pocket-image-test-");
        byte[] body=new byte[200000];new Random(17).nextBytes(body);String digest=sha(body);
        AtomicLong range=new AtomicLong(-1);HttpServer server=HttpServer.create(new InetSocketAddress("127.0.0.1",0),0);
        server.createContext("/range",exchange->{
            String value=exchange.getRequestHeaders().getFirst("Range");int start=value==null?0:Integer.parseInt(value.substring(6,value.length()-1));range.set(start);
            if(start>0)exchange.getResponseHeaders().set("Content-Range","bytes "+start+"-"+(body.length-1)+"/"+body.length);
            exchange.sendResponseHeaders(start>0?206:200,body.length-start);
            try(OutputStream out=exchange.getResponseBody()){out.write(body,start,body.length-start);}
        });
        server.createContext("/restart",exchange->{exchange.sendResponseHeaders(200,body.length);try(OutputStream out=exchange.getResponseBody()){out.write(body);}});
        server.createContext("/bad-range",exchange->{exchange.getResponseHeaders().set("Content-Range","bytes 0-1/2");exchange.sendResponseHeaders(206,2);try(OutputStream out=exchange.getResponseBody()){out.write(new byte[2]);}});
        server.createContext("/redirect",exchange->{exchange.getResponseHeaders().set("Location","/range");exchange.sendResponseHeaders(302,-1);exchange.close();});
        server.start();String base="http://127.0.0.1:"+server.getAddress().getPort();
        try {
            File cache=temp.resolve("resume").toFile();cache.mkdir();Files.write(new File(cache,digest+".part").toPath(),Arrays.copyOf(body,12345));
            File ready=ImageDownload.fetch(new URL(base+"/range"),cache,body.length,digest,PROGRESS);
            check(range.get()==12345,"Download did not resume with Range");check(Arrays.equals(body,Files.readAllBytes(ready.toPath())),"Resumed bytes differ");
            File restart=temp.resolve("restart").toFile();restart.mkdir();Files.write(new File(restart,digest+".part").toPath(),Arrays.copyOf(body,25));
            check(ImageDownload.sha256(ImageDownload.fetch(new URL(base+"/restart"),restart,body.length,digest,PROGRESS)).equals(digest),"200 response did not restart");
            File badRange=temp.resolve("bad-range").toFile();badRange.mkdir();Files.write(new File(badRange,digest+".part").toPath(),Arrays.copyOf(body,25));
            rejected(()->ImageDownload.fetch(new URL(base+"/bad-range"),badRange,body.length,digest,PROGRESS));
            String wrong=sha(new byte[]{1});File corrupt=temp.resolve("corrupt").toFile();
            rejected(()->ImageDownload.fetch(new URL(base+"/range"),corrupt,body.length,wrong,PROGRESS));
            check(!new File(corrupt,wrong+".zip").exists() && !new File(corrupt,wrong+".part").exists(),"Corrupt download was retained");
            check(ImageDownload.fetch(new URL(base+"/redirect"),temp.resolve("redirect").toFile(),body.length,digest,PROGRESS).isFile(),"Redirect failed");
            rejected(()->ImageDownload.checkUrl(new URL("http://example.com/image.zip")));
            rejected(()->ImageDownload.checkUrl(new URL("https://user:secret@example.com/image.zip")));
        } finally {server.stop(0);}
        File cached=ImageDownload.fetch(new URL(base+"/range"),temp.resolve("resume").toFile(),body.length,digest,PROGRESS);
        check(cached.isFile(),"Verified cached image should work offline");

        Map<String,byte[]> files=new TreeMap<>();files.put("system/bin/init",new byte[]{1,2,3});files.put("vendor/etc/config",new byte[]{4,5});
        files.put("apex/com.android.runtime/apex_manifest.json",new byte[]{6,7});
        Map<String,ImageArchive.Entry> entries=specs(files);File archive=zip(temp,files);File root=temp.resolve("root").toFile();root.mkdirs();
        Path data=root.toPath().resolve("system-data/private");Files.createDirectories(data.getParent());Files.write(data,new byte[]{99});
        ImageArchive.install(archive,root,entries,sha(MANIFEST),digest,PROGRESS);
        check(Files.readAllBytes(data)[0]==99,"Image update changed guest data");
        check(Arrays.equals(files.get("system/bin/init"),Files.readAllBytes(root.toPath().resolve("system/bin/init"))),"Installed bytes differ");
        check(Arrays.equals(files.get("apex/com.android.runtime/apex_manifest.json"),Files.readAllBytes(root.toPath().resolve("apex/com.android.runtime/apex_manifest.json"))),"APEX payload was not installed");
        check(Files.isExecutable(root.toPath().resolve("system/bin/init")),"Executable permission lost");
        Map<String,ImageArchive.Entry> corruptSpecs=new TreeMap<>(entries);
        ImageArchive.Entry spec=entries.get("vendor/etc/config");corruptSpecs.put("vendor/etc/config",new ImageArchive.Entry(spec.size,spec.crc,sha(new byte[]{9}),spec.mode));
        Files.write(root.toPath().resolve("system/bin/init"),new byte[]{77});
        rejected(()->ImageArchive.install(archive,root,corruptSpecs,sha(MANIFEST),digest,PROGRESS));
        check(Files.readAllBytes(root.toPath().resolve("system/bin/init"))[0]==77,"Validation failure replaced an installed file");
        rejected(()->ImageArchive.path("system/../system-data/private"));
        rejected(()->ImageArchive.path("apex/../../system-data/private"));
        rejected(()->ImageArchive.path("system-data/private"));
        rejected(()->ImageArchive.path("/system/bin/init"));rejected(()->ImageArchive.path("system//bin/init"));
        rejected(()->ImageArchive.path("system\\bin\\init"));
        File linked=temp.resolve("linked").toFile();linked.mkdir();Path outside=Files.createDirectory(temp.resolve("outside"));
        Files.createSymbolicLink(linked.toPath().resolve("system"),outside);
        rejected(()->ImageArchive.install(archive,linked,entries,sha(MANIFEST),digest,PROGRESS));
        check(!Files.exists(outside.resolve("bin/init")),"Archive followed a symlink");
        rejected(()->ImageArchive.install(archive,root,entries,sha(new byte[]{7}),digest,PROGRESS));
        File recovery=temp.resolve("recovery").toFile();recovery.mkdir();
        rejected(()->ImageArchive.install(archive,recovery,entries,sha(MANIFEST),digest,(done,total)->{if(done>0)Thread.currentThread().interrupt();}));
        Thread.interrupted();check(new File(recovery,".pocket-image-installing").exists(),"Interrupted install lacks recovery marker");
        ImageArchive.install(archive,recovery,entries,sha(MANIFEST),digest,PROGRESS);
        check(new File(recovery,"vendor/etc/config").isFile(),"Interrupted install did not recover");
        System.out.println("PASS: "+assertions+" image download/archive assertions");
    }
}
