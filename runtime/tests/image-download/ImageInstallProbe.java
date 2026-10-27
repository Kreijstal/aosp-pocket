// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;
import org.json.*;

/** Fresh Android install, offline reuse and migration; never starts a guest. */
public final class ImageInstallProbe extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);TextView status=new TextView(this);status.setText("Testing image download…");setContentView(status);
        new Thread(()->{
            JSONObject report=new JSONObject();
            try {
                File root=new File(getFilesDir(),"api28-native-rootfs");File data=new File(root,"system-data/preservation-marker");
                data.getParentFile().mkdirs();
                if(!data.exists())Files.write(data.toPath(),"keep-me".getBytes("UTF-8"));
                if(!new String(Files.readAllBytes(data.toPath()),"UTF-8").equals("keep-me"))throw new AssertionError("Existing preservation marker changed");
                boolean resumed=new File(root,".pocket-image-id").isFile();
                int expectedDownloads=resumed?0:1;
                int[] downloads={0};List<String> phases=new ArrayList<>();
                GuestImage.Progress progress=new GuestImage.Progress(){
                    public void update(long done,long total){}
                    public void phase(String name){phases.add(name);if(name.equals("Downloading Android 9"))downloads[0]++;}
                };
                if(!RemoteImage.prepare(this,root,progress))throw new AssertionError("No downloadable image configured");
                if(downloads[0]!=expectedDownloads)throw new AssertionError("Unexpected download count");
                JSONObject catalog;
                try(InputStream in=getAssets().open("pocket-images.json")) {
                    ByteArrayOutputStream bytes=new ByteArrayOutputStream();GuestImage.copy(in,bytes,8*1024*1024);
                    catalog=new JSONObject(bytes.toString("UTF-8")).getJSONArray("images").getJSONObject(0);
                }
                JSONObject entries=catalog.getJSONObject("manifest").getJSONObject("entries");int checked=0;
                for(Iterator<String> it=entries.keys();it.hasNext();) {
                    String name=it.next();File file=new File(root,name);
                    if(!ImageDownload.sha256(file).equals(entries.getJSONObject(name).getString("sha256")))throw new AssertionError("Installed hash differs: "+name);
                    if((android.system.Os.stat(file.getPath()).st_mode&0777)!=entries.getJSONObject(name).getInt("mode"))throw new AssertionError("Installed mode differs: "+name);
                    checked++;
                }
                int bundled=0;
                try(ZipFile apk=new ZipFile(getApplicationInfo().sourceDir)) {
                    for(Enumeration<? extends ZipEntry> all=apk.entries();all.hasMoreElements();) {
                        String name=all.nextElement().getName();if(name.startsWith("assets/api28-rootfs/") || name.startsWith("assets/default-guest/"))bundled++;
                    }
                }
                if(bundled!=0)throw new AssertionError("Host APK still contains an image");
                if(!RemoteImage.prepare(this,root,progress) || downloads[0]!=expectedDownloads)throw new AssertionError("Offline reuse attempted another download");
                Files.delete(new File(root,".pocket-image-id").toPath());
                Files.delete(new File(root,".pocket-image-manifest.json").toPath());
                if(!RemoteImage.prepare(this,root,progress) || downloads[0]!=expectedDownloads)throw new AssertionError("Existing image migration attempted another download");
                if(!new String(Files.readAllBytes(data.toPath()),"UTF-8").equals("keep-me"))throw new AssertionError("Image setup changed guest data");
                report.put("validation","PASS").put("scope","Pi Android; isolated probe package; actual HTTP download and install")
                    .put("downloadCount",downloads[0]).put("resumedProbeVerification",resumed).put("verifiedFiles",checked).put("filePermissionsVerified",true).put("bundledImageEntries",bundled)
                    .put("offlineReuse",true).put("legacyMigrationWithoutDownload",true).put("guestDataPreserved",true)
                    .put("imageSha256",catalog.getString("sha256")).put("phases",new JSONArray(phases));
            } catch(Throwable error){try{report.put("validation","FAIL").put("error",error.toString());}catch(JSONException ignored){}}
            try(FileOutputStream out=openFileOutput("result.json",0)){out.write(report.toString().getBytes("UTF-8"));}
            catch(IOException error){throw new RuntimeException(error);}
            String result=report.toString();runOnUiThread(()->status.setText(result));
        },"ImageInstallProbe").start();
    }
}
