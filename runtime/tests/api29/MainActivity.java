// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.api29test;
import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
import java.io.*;
import java.nio.file.*;
import java.util.concurrent.TimeUnit;
import android.system.Os;
import android.system.OsConstants;
import android.system.ErrnoException;
import android.content.SharedPreferences;
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.security.MessageDigest;
import org.json.JSONObject;
import dev.kreijstal.aosppocket.runtime.HostBinder;

/** Separate sandbox/package/data: the working Android 9 profile is untouched. */
public final class MainActivity extends Activity {
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);TextView status=new TextView(this);status.setText("Android 10 runtime test…");setContentView(status);
        new Thread(()->{
            JSONObject report=new JSONObject();
            HostBinder binder=null;
            File root=new File(getFilesDir(),"root");
            try {
                stopGuest(root);
                try(java.net.DatagramSocket socket=new java.net.DatagramSocket(null)){
                    report.put("hostDatagramSocket",true);
                }catch(Exception error){report.put("hostDatagramSocket",error.toString());}
                report.put("extractedFiles",extract(root));
                report.put("imageVerifiedFiles",verifyImage(root));
                for(String name:new String[]{".pocket-owner","data/local/tmp","data/dalvik-cache/arm64","dev/__properties__","dev/socket","tmp","mnt/asec","mnt/expand"})
                    if(!new File(root,name).isDirectory() && !new File(root,name).mkdirs())throw new IOException("Cannot create runtime directory");
                Path etc=new File(root,"etc").toPath();
                if(!Files.exists(etc,LinkOption.NOFOLLOW_LINKS))Files.createSymbolicLink(etc,Paths.get("system/etc"));
                Path product=new File(root,"product").toPath();
                if(!Files.exists(product,LinkOption.NOFOLLOW_LINKS))Files.createSymbolicLink(product,Paths.get("system/product"));
                // Property areas belong to one boot. Old read-only values must
                // not survive a restart of this isolated fixture.
                File[] oldProperties=new File(root,"dev/__properties__").listFiles();
                if(oldProperties==null)throw new IOException("Cannot access fixture properties");
                for(File entry:oldProperties)
                    if(!entry.delete())throw new IOException("Cannot clear fixture property area");
                if(new String(Files.readAllBytes(new File(root,"probe-binder-enabled").toPath()),"UTF-8").equals("1"))
                    binder=new HostBinder(this,root);
                File log=new File(getFilesDir(),"native.log");
                ProcessBuilder builder=new ProcessBuilder(new File(root,"system/bin/sh").getPath(),"/probe.sh").directory(root);
                builder.environment().clear();
                for(String[] env:new String[][]{{"POCKET_LOADER_BOOT","1"},{"ANDROID_ROOT","/system"},{"ANDROID_DATA","/data"},
                        {"ANDROID_RUNTIME_ROOT","/apex/com.android.runtime"},{"ANDROID_TZDATA_ROOT","/apex/com.android.tzdata"},{"EXTERNAL_STORAGE","/sdcard"},
                        {"PATH","/system/bin:/apex/com.android.runtime/bin"}})builder.environment().put(env[0],env[1]);
                String[] props=new String(Files.readAllBytes(new File(root,"probe-environment").toPath()),"UTF-8").split("\n");
                for(String entry:props){int split=entry.indexOf('=');if(split>0)builder.environment().put(entry.substring(0,split),entry.substring(split+1));}
                builder.redirectInput(new File("/dev/null")).redirectErrorStream(true).redirectOutput(log);
                Process child=builder.start();
                boolean finished=child.waitFor(builder.environment().containsKey("POCKET_API29_UI_PROBE")?300:120,TimeUnit.SECONDS);
                if(!finished){
                    child.destroy();
                    if(!child.waitFor(5,TimeUnit.SECONDS)){child.destroyForcibly();child.waitFor(5,TimeUnit.SECONDS);}
                }
                String output=new String(Files.readAllBytes(log.toPath()),"UTF-8");
                File initLog=new File(root,"tmp/properties.log");
                if(!output.contains("API29_PROPERTIES_READY") && initLog.isFile())
                    output+="\n"+new String(Files.readAllBytes(initLog.toPath()),"UTF-8");
                report.put("hostUid",android.os.Process.myUid()).put("exitCode",child.isAlive()?-1:child.exitValue()).put("log",output)
                    .put("passed",finished && child.exitValue()==0 && output.contains("API29_NATIVE_RUNTIME_PASS"))
                    .put("scope","Isolated Android 10 bring-up; full guest UI/app boot is not yet verified");
                if(!finished)report.put("error","Android 10 probe timed out");
            }catch(Throwable error){try{report.put("passed",false).put("error",error.toString());}catch(Exception ignored){}}
            finally{
                try{stopGuest(root);}catch(Exception error){try{report.put("passed",false).put("cleanupError",error.toString());}catch(Exception ignored){}}
                if(binder!=null)binder.close();
            }
            try{Files.write(new File(getFilesDir(),"report.json").toPath(),report.toString(2).getBytes("UTF-8"));}catch(Exception ignored){}
            runOnUiThread(()->status.setText(report.toString()));
        },"PocketApi29Probe").start();
    }
    /** Kill only executables inside this fixture's private root, as its app UID. */
    private void stopGuest(File root)throws Exception {
        String prefix=root.getCanonicalPath()+"/";
        for(int attempt=0;attempt<40;attempt++){
            File[] entries=new File("/proc").listFiles();
            if(entries==null)throw new IOException("Cannot inspect fixture processes");
            boolean found=false;
            for(File entry:entries){
                if(!entry.getName().matches("\\d+"))continue;
                try{
                    if(Os.stat(entry.getPath()).st_uid!=android.os.Process.myUid())continue;
                    if(!Os.readlink(new File(entry,"exe").getPath()).startsWith(prefix))continue;
                    found=true;
                    Os.kill(Integer.parseInt(entry.getName()),attempt<20?OsConstants.SIGTERM:OsConstants.SIGKILL);
                }catch(ErrnoException exited){/* Exited or inaccessible. */}
            }
            if(!found)return;
            Thread.sleep(100);
        }
        throw new IOException("Fixture guest processes did not exit");
    }
    private int extract(File root)throws Exception {
        if(Files.isSymbolicLink(root.toPath()))throw new IOException("Unexpected fixture root symlink");
        if(!root.isDirectory() && !root.mkdirs())throw new IOException("Cannot create fixture root");
        SharedPreferences cache=getSharedPreferences("fixture-assets",MODE_PRIVATE);
        SharedPreferences.Editor stamps=cache.edit();int copied=0;
        String prefix="assets/root/",base=root.getCanonicalPath()+"/";
        // The source is the installed, signed APK. Cache its CRC and size just
        // like Manager's bundled-image extraction, avoiding hundreds of MB of
        // repeated writes when only a service adapter changed.
        try(ZipFile apk=new ZipFile(getApplicationInfo().sourceDir)){
            Enumeration<? extends ZipEntry> entries=apk.entries();
            while(entries.hasMoreElements()){
                ZipEntry entry=entries.nextElement();
                if(!entry.getName().startsWith(prefix) || entry.isDirectory())continue;
                String name=entry.getName().substring(prefix.length());
                for(String part:name.split("/",-1))
                    if(part.isEmpty() || part.equals(".") || part.equals("..") || part.indexOf('\\')>=0)
                        throw new IOException("Invalid fixture asset path");
                File target=new File(root,name);
                if(!target.getCanonicalPath().startsWith(base))throw new IOException("Fixture asset escapes root");
                for(File parent=target;!parent.equals(root);parent=parent.getParentFile())
                    if(Files.isSymbolicLink(parent.toPath()))throw new IOException("Unexpected fixture symlink");
                String stamp=entry.getCrc()+":"+entry.getSize();
                if(stamp.equals(cache.getString(name,"")) && target.isFile() && target.length()==entry.getSize())continue;
                if(!target.getParentFile().isDirectory() && !target.getParentFile().mkdirs())throw new IOException("Cannot create fixture directory");
                File temporary=new File(target.getPath()+".new");
                if(Files.isSymbolicLink(temporary.toPath()))throw new IOException("Unexpected fixture temporary symlink");
                long written=0;
                try(InputStream in=apk.getInputStream(entry);OutputStream out=new FileOutputStream(temporary)){
                    byte[] buffer=new byte[65536];int count;
                    while((count=in.read(buffer))!=-1){out.write(buffer,0,count);written+=count;}
                }
                if(written!=entry.getSize())throw new IOException("Incomplete fixture asset");
                Os.chmod(temporary.getPath(),0755);
                Files.move(temporary.toPath(),target.toPath(),StandardCopyOption.REPLACE_EXISTING,StandardCopyOption.ATOMIC_MOVE);
                stamps.putString(name,stamp);copied++;
            }
        }
        if(!stamps.commit())throw new IOException("Cannot save fixture asset metadata");
        return copied;
    }
    private File imageFile(File root,String name)throws Exception {
        if(name.indexOf('\0')>=0)throw new IOException("Invalid fixture image path");
        for(String part:name.split("/",-1))
            if(part.isEmpty() || part.equals(".") || part.equals("..") || part.indexOf('\\')>=0)
                throw new IOException("Invalid fixture image path");
        File target=new File(root,name);
        if(!target.getCanonicalPath().startsWith(root.getCanonicalPath()+"/"))throw new IOException("Fixture image escapes root");
        for(File parent=target;!parent.equals(root);parent=parent.getParentFile())
            if(Files.isSymbolicLink(parent.toPath()))throw new IOException("Fixture image contains a symlink");
        return target;
    }
    private int verifyImage(File root)throws Exception {
        JSONObject manifest;
        try(InputStream input=getAssets().open("fixture-image.json");ByteArrayOutputStream output=new ByteArrayOutputStream()){
            byte[] buffer=new byte[32768];int count;
            while((count=input.read(buffer))!=-1){if(output.size()+count>8*1024*1024)throw new IOException("Fixture manifest too large");output.write(buffer,0,count);}
            manifest=new JSONObject(new String(output.toByteArray(),"UTF-8"));
        }
        if(manifest.getInt("schema")!=1 || manifest.getInt("sdk")!=29)throw new IOException("Unsupported fixture image");
        JSONObject files=manifest.getJSONObject("files");
        if(files.length()==0 || files.length()>20000)throw new IOException("Invalid fixture file count");
        java.util.Iterator<String> names=files.keys();long total=0;
        while(names.hasNext()){
            String name=names.next();JSONObject spec=files.getJSONObject(name);long size=spec.getLong("bytes");
            String expected=spec.getString("sha256");
            if(size<0 || size>2L*1024*1024*1024-total || !expected.matches("[0-9a-f]{64}"))throw new IOException("Invalid fixture file metadata");
            total+=size;File file=imageFile(root,name);
            if(!file.isFile() || file.length()!=size)throw new IOException("Missing or incomplete fixture file: "+name);
            MessageDigest hash=MessageDigest.getInstance("SHA-256");long read=0;
            try(InputStream input=new FileInputStream(file)){
                byte[] buffer=new byte[65536];int count;
                while((count=input.read(buffer))!=-1){read+=count;if(read>size)throw new IOException("Fixture file changed: "+name);hash.update(buffer,0,count);}
            }
            StringBuilder actual=new StringBuilder();
            for(byte value:hash.digest())actual.append("0123456789abcdef".charAt((value&255)>>>4)).append("0123456789abcdef".charAt(value&15));
            if(read!=size || !expected.equals(actual.toString()))throw new IOException("Fixture image checksum differs: "+name);
        }
        return files.length();
    }
}
