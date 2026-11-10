// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.content.*;
import android.os.Build;
import java.io.*;
import java.net.URL;
import java.nio.file.*;
import java.util.*;
import org.json.*;

/** The signed host APK pins the download URL, image hash and complete file set. */
final class RemoteImage {
    private static final String CATALOG="pocket-images.json";
    private static final Map<Integer,JSONObject> selected=new HashMap<>();
    private static JSONObject catalog;
    private static boolean catalogLoaded;
    static boolean available(Context context) {
        return available(context,28);
    }
    static boolean available(Context context,int sdk) {
        try{return descriptor(context,sdk)!=null;}catch(Exception invalid){return false;}
    }
    private static String read(InputStream in,int limit)throws IOException {
        ByteArrayOutputStream out=new ByteArrayOutputStream();byte[] buffer=new byte[16384];
        for(int n;(n=in.read(buffer))!=-1;){if(n>limit-out.size())throw new IOException("Image metadata exceeds size limit");out.write(buffer,0,n);}
        return out.toString("UTF-8");
    }
    private static synchronized JSONObject descriptor(Context context,int sdk)throws Exception {
        if(sdk!=28 && sdk!=29 && sdk!=32)throw new IOException("Unsupported direct guest SDK");
        if(selected.containsKey(sdk))return selected.get(sdk);
        if(!catalogLoaded){
            String text;
            try(InputStream in=context.getAssets().open(CATALOG)){text=read(in,8*1024*1024);}
            catch(FileNotFoundException absent){catalogLoaded=true;return null;}
            catalog=new JSONObject(text);catalogLoaded=true;
        }
        if(catalog==null)return null;
        if(catalog.getInt("schema")!=1)throw new IOException("Unsupported image catalog");
        JSONArray images=catalog.getJSONArray("images");JSONObject match=null;
        for(int i=0;i<images.length();i++) {
            JSONObject image=images.getJSONObject(i);
            if(image.getString("profile").equals("api"+sdk+"-direct") && image.getInt("sdk")==sdk && image.getInt("runtime")==1
                    && image.getString("abi").equals("arm64-v8a") && Arrays.asList(Build.SUPPORTED_ABIS).contains("arm64-v8a")) {
                if(match!=null)throw new IOException("Duplicate Android image profile");match=image;
            }
        }
        selected.put(sdk,match);return match;
    }
    static boolean prepare(Context context,File root,GuestImage.Progress progress)throws Exception {
        return prepare(context,root,28,progress);
    }
    static boolean prepare(Context context,File root,int sdk,GuestImage.Progress progress)throws Exception {
        JSONObject image=descriptor(context,sdk);if(image==null)return false;
        String androidVersion=sdk==32?"12L":sdk==29?"10":"9";
        JSONObject manifest=image.getJSONObject("manifest");
        for(String field:new String[]{"profile","sdk","abi","runtime","version"})
            if(!image.get(field).toString().equals(manifest.get(field).toString()))throw new IOException("Image metadata mismatch");
        if(manifest.getInt("schema")!=1)throw new IOException("Unsupported image manifest");
        if(sdk==32) {
            for(JSONObject metadata:new JSONObject[]{image,manifest})
                if(!metadata.getBoolean("experimental") || metadata.getInt("service_layout")!=2
                        || !metadata.getString("boot_protocol").equals("api32-v1")
                        || !metadata.getString("storage").equals("app-private"))
                    throw new IOException("Unsupported Android 12L runtime layout");
            if(image.has("child_supervision") || manifest.has("child_supervision"))
                if(!"kernel-subreaper".equals(image.optString("child_supervision"))
                        || !"kernel-subreaper".equals(manifest.optString("child_supervision")))
                    throw new IOException("Unsupported Android 12L child supervision");
        }
        String sha=image.getString("sha256"),manifestSha=image.getString("manifest_sha256");
        if(!sha.matches("[0-9a-f]{64}") || !manifestSha.matches("[0-9a-f]{64}"))throw new IOException("Invalid image checksum");
        long packed=image.getLong("bytes"),unpacked=image.getLong("unpacked_bytes");
        if(packed<=0 || packed>ImageArchive.LIMIT || unpacked<=0 || unpacked>ImageArchive.LIMIT)throw new IOException("Invalid image size");
        JSONObject entries=manifest.getJSONObject("entries");
        if(entries.length()==0 || entries.length()>20000)throw new IOException("Invalid image file count");
        Map<String,ImageArchive.Entry> files=new TreeMap<>();long total=0;
        for(Iterator<String> it=entries.keys();it.hasNext();) {
            String name=it.next();ImageArchive.path(name);JSONObject spec=entries.getJSONObject(name);
            ImageArchive.Entry entry=new ImageArchive.Entry(spec.getLong("size"),spec.getLong("crc32"),spec.getString("sha256"),spec.getInt("mode"));
            if(entry.size>ImageArchive.LIMIT-total)throw new IOException("Image exceeds size limit");total+=entry.size;files.put(name,entry);
        }
        if(total!=unpacked)throw new IOException("Image total size differs");
        for(String required:new String[]{"system/bin/pocket-init-api"+sdk,"system/bin/pocket-loader","system/build.prop","system/usr/keychars/Virtual.kcm"})
            if(!files.containsKey(required))throw new IOException("Image lacks a required system file");
        if(sdk==29 && !files.containsKey("apex/com.android.runtime/lib64/bionic/libc.so"))
            throw new IOException("Android 10 image lacks its runtime APEX");
        if(sdk==32)for(String required:new String[]{".pocket-readonly-system","bootstrap-linkerconfig",
                "system/bin/pocket-core-api32","system/bin/pocket-crypto-api32",
                "system/bin/pocket-runtime-prepare-api32","system/bin/pocket-runtime-wait-api32",
                "system/bin/getprop","system/bin/pocket-setprop-api32",
                "system/bin/pocket-manager-api32","system/framework/pocket-manager.jar",
                "system/etc/init/pocket-runtime.rc","system/etc/pocket-runtime-boot.sh","system/etc/pocket-runtime-env.sh",
                "system/framework/pocket-zygote.jar","system/framework/pocket-tools.jar",
                "apex/com.android.art/javalib/arm64/boot.art","apex/com.android.runtime/lib64/bionic/libc.so",
                "product/overlay/PocketMemory/PocketMemory.apk"})
            if(!files.containsKey(required))throw new IOException("Android 12L image lacks "+required);
        if(!root.isDirectory() && !root.mkdirs())throw new IOException("Cannot create image directory");
        File marker=new File(root,".pocket-image-id"),pending=new File(root,".pocket-image-installing");
        boolean complete=!pending.exists();
        if(complete)for(String name:files.keySet())if(!ImageArchive.target(root,name).isFile()){complete=false;break;}
        if(complete && marker.isFile())try(InputStream in=new FileInputStream(marker)) {
            if(read(in,128).trim().equals(sha))return true;
        }
        SharedPreferences cache=context.getSharedPreferences("manager-api"+sdk+"-image",Context.MODE_PRIVATE);
        // A matching old APK already installed this exact file inventory. Keep
        // its runtime-patched ELF files and existing data, including offline.
        boolean adopt=complete && !marker.exists();
        if(adopt)for(Map.Entry<String,ImageArchive.Entry> item:files.entrySet()) {
            ImageArchive.Entry spec=item.getValue();
            if(!(spec.crc+":"+spec.size).equals(cache.getString(item.getKey(),""))){adopt=false;break;}
        }
        File savedManifest=new File(root,".pocket-image-manifest.json");
        if(adopt) {
            ImageArchive.atomicText(savedManifest,manifest.toString()+"\n");ImageArchive.atomicText(marker,sha+"\n");return true;
        }
        File downloads=new File(context.getFilesDir(),"guest-images");
        File partial=new File(downloads,sha+".part"),archive=new File(downloads,sha+".zip");
        long remaining=archive.isFile()?0:Math.max(0,packed-(partial.isFile()?partial.length():0));
        // Installed files are replaced one at a time after complete validation;
        // no second full unpacked image is needed alongside existing /data.
        long largest=0,missing=0;
        for(Map.Entry<String,ImageArchive.Entry> item:files.entrySet()) {
            File target=ImageArchive.target(root,item.getKey());long size=item.getValue().size;
            largest=Math.max(largest,size);missing+=Math.max(0,size-(target.isFile()?target.length():0));
        }
        if(context.getFilesDir().getUsableSpace()<remaining+missing+largest+32L*1024*1024)
            throw new IOException("Not enough storage to download and set up Android "+androidVersion);
        progress.phase("Downloading Android "+androidVersion);
        File payload=ImageDownload.fetch(new URL(image.getString("url")),downloads,packed,sha,progress::update);
        progress.phase("Checking Android "+androidVersion);
        boolean[] verified={false};
        long installStarted=System.nanoTime();
        ImageArchive.InstallStats installed=ImageArchive.install(payload,root,files,manifestSha,sha,(done,size)->{
            if(!verified[0]){verified[0]=true;progress.phase("Setting up Android "+androidVersion);}
            progress.update(done,size);
        });
        JSONObject statistics=new JSONObject().put("schema",1).put("sdk",sdk).put("imageSha256",sha)
            .put("reusedFiles",installed.reusedFiles).put("replacedFiles",installed.replacedFiles)
            .put("reusedBytes",installed.reusedBytes).put("writtenBytes",installed.writtenBytes)
            .put("durationMillis",(System.nanoTime()-installStarted)/1000000L);
        ImageArchive.atomicText(new File(root,".pocket-image-install-stats.json"),statistics.toString()+"\n");
        if(savedManifest.isFile()) {
            JSONObject old;
            try(InputStream in=new FileInputStream(savedManifest)){old=new JSONObject(read(in,8*1024*1024)).getJSONObject("entries");}
            for(Iterator<String> it=old.keys();it.hasNext();) {
                String name=it.next();if(!files.containsKey(name))Files.deleteIfExists(ImageArchive.target(root,name).toPath());
            }
        }
        SharedPreferences.Editor stamps=cache.edit().clear();
        for(Map.Entry<String,ImageArchive.Entry> item:files.entrySet())stamps.putString(item.getKey(),item.getValue().crc+":"+item.getValue().size);
        if(!stamps.commit())throw new IOException("Cannot save installed image metadata");
        ImageArchive.atomicText(savedManifest,manifest.toString()+"\n");ImageArchive.atomicText(marker,sha+"\n");
        Files.deleteIfExists(pending.toPath());
        // The installed image supports offline boot without retaining another
        // compressed copy. An interrupted installation keeps this cache.
        Files.deleteIfExists(payload.toPath());return true;
    }
}
