// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.content.*;
import android.system.Os;
import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

/** The pinned direct Android 9 image. Never modifies the API 25 rootfs/data. */
final class DirectImage {
    static boolean bundled(Context context) {
        try(InputStream in=context.getAssets().open("api28-rootfs/system/build.prop.payload")){return in.read()!=-1;}
        catch(IOException missing){return false;}
    }
    static File prepare(Context context,GuestImage.Progress progress) throws Exception {
        File root=new File(context.getFilesDir(),"api28-rootfs");root.mkdirs();
        try (InputStream marker=context.getAssets().open("manager-dev-update.json")) {
            if (!new File(root,"system/bin/pocket-init-api28").isFile())
                throw new IllegalStateException("Development host update requires an installed full Manager image");
        } catch (FileNotFoundException standalone) { /* Full distribution APK. */ }
        android.content.SharedPreferences cache=context.getSharedPreferences("manager-api28-image",Context.MODE_PRIVATE);
        android.content.SharedPreferences.Editor stamps=cache.edit();
        try(ZipFile apk=new ZipFile(context.getApplicationInfo().sourceDir)) {
            List<ZipEntry> payload=new ArrayList<>();long total=0,done=0;
            for(Enumeration<? extends ZipEntry> all=apk.entries();all.hasMoreElements();) {
                ZipEntry entry=all.nextElement();
                if(entry.getName().startsWith("assets/api28-rootfs/")){payload.add(entry);total+=entry.getSize();}
            }
            if(payload.isEmpty())throw new IOException("Android 9 image is not bundled in this APK");
            for(ZipEntry entry:payload) {
                if(Thread.currentThread().isInterrupted())throw new InterruptedIOException("Image setup cancelled");
                String name=entry.getName().substring("assets/api28-rootfs/".length());
                if(!name.endsWith(".payload"))throw new IOException("Invalid image entry");
                name=name.substring(0,name.length()-8);
                if(!(name.startsWith("system/") || name.startsWith("vendor/")) || name.contains("..") || name.contains("\\"))
                    throw new IOException("Invalid Android 9 image path");
                File target=new File(root,name);
                if(!target.getCanonicalPath().startsWith(root.getCanonicalPath()+"/"))throw new IOException("Image path escapes root");
                for(File parent=target;!parent.equals(root);parent=parent.getParentFile())
                    if(Files.isSymbolicLink(parent.toPath()))throw new IOException("Image path contains a symlink");
                String stamp=entry.getCrc()+":"+entry.getSize();
                if(!stamp.equals(cache.getString(name,"")) || !target.isFile() || target.length()!=entry.getSize()) {
                    target.getParentFile().mkdirs();File temp=new File(target.getPath()+".new");
                    try(InputStream in=apk.getInputStream(entry);OutputStream out=new FileOutputStream(temp)) {
                        GuestImage.copy(in,out,2L*1024*1024*1024);
                    }
                    Os.chmod(temp.getPath(),0755);
                    Files.move(temp.toPath(),target.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
                    stamps.putString(name,stamp);
                }
                done+=entry.getSize();progress.update(done,total);
            }
        }
        stamps.commit();
        if(GuestProfile.detect(root).sdk!=28)throw new IOException("Direct backend requires the pinned Android 9 image");
        for(String name:new String[]{"system-data","dev/socket","dev/__properties__","logs","tmp"})new File(root,name).mkdirs();
        File data=new File(root,"data");
        if(Files.isSymbolicLink(data.toPath()))Files.delete(data.toPath());
        else if(data.exists())throw new IOException("Unexpected Android 9 data directory; refusing to replace it");
        Os.symlink("system-data",data.getPath());
        for(File entry:new File(root,"dev/__properties__").listFiles())
            if(!entry.delete())throw new IOException("Cannot reset guest property area");
        File console=new File(root,"pocket-console");Files.deleteIfExists(console.toPath());
        Os.symlink("logs/manager-init.log",console.getPath());
        for(File executable:new File(root,"system/bin").listFiles())patchInterpreter(executable,new File(root,"system/bin/linker64").getPath());
        Files.write(new File(root,"system/etc/ld.config.txt").toPath(),("dir.pocket = "+root+"\n[pocket]\nnamespace.default.isolated = false\nnamespace.default.search.paths = "+root+"/system/lib64:"+root+"/vendor/lib64\nnamespace.default.permitted.paths = "+root+"\n").getBytes("UTF-8"));
        GuestBinder.preparePlatform(context,root);
        return root;
    }
    private static void patchInterpreter(File executable,String interpreter) throws Exception {
        byte[] elf=Files.readAllBytes(executable.toPath());
        if(elf.length<64 || elf[0]!=127 || elf[1]!='E' || elf[2]!='L' || elf[3]!='F')return;
        ByteBuffer b=ByteBuffer.wrap(elf).order(ByteOrder.LITTLE_ENDIAN);
        int table=Math.toIntExact(b.getLong(32)),size=b.getShort(54)&65535,count=b.getShort(56)&65535;
        for(int n=0;n<count;n++) {
            int pos=table+n*size;if(b.getInt(pos)!=3)continue;
            int offset=Math.toIntExact(b.getLong(pos+8)),capacity=Math.toIntExact(b.getLong(pos+32));
            byte[] path=interpreter.getBytes("UTF-8");
            if(capacity<path.length+1 || offset<0 || capacity>elf.length-offset)throw new IOException("Invalid interpreter reservation");
            Arrays.fill(elf,offset,offset+capacity,(byte)0);System.arraycopy(path,0,elf,offset,path.length);
            Files.write(executable.toPath(),elf);return;
        }
    }
    static java.lang.Process start(File root) throws IOException {
        ProcessBuilder builder=new ProcessBuilder(new File(root,"system/bin/pocket-init-api28").getPath(),"/system/etc/init/init.manager.rc").directory(root);
        Map<String,String> env=builder.environment();
        for(String name:new String[]{"LD_PRELOAD","LD_LIBRARY_PATH","BOOTCLASSPATH","DEX2OATBOOTCLASSPATH","SYSTEMSERVERCLASSPATH","ANDROID_ART_ROOT","ANDROID_RUNTIME_ROOT","ANDROID_I18N_ROOT","ANDROID_TZDATA_ROOT"})env.remove(name);
        String libs=root+"/system/lib64:"+root+"/vendor/lib64";
        env.put("POCKET_DIRECT_ROOT",root.getPath());env.put("POCKET_DIRECT_LIBRARIES",libs);env.put("POCKET_DIRECT_ID","0:0");
        env.put("LD_LIBRARY_PATH",libs);env.put("LD_CONFIG_FILE",root+"/system/etc/ld.config.txt");
        env.put("LD_PRELOAD",root+"/system/lib64/libpocket_direct_boot.so:"+root+"/system/lib64/libpocket_binder_api28.so:"+root+"/system/lib64/libpocket_hidl_api28.so");
        env.put("POCKET_PARENT_PID",Integer.toString(android.os.Process.myPid()));
        env.put("POCKET_DIRECT_CONSOLE",root+"/logs/manager-init.log");env.put("ANDROID_ROOT","/system");env.put("ANDROID_DATA","/data");env.put("TMPDIR","/tmp");
        File log=new File(root,"logs/manager-init.log");Files.write(log.toPath(),new byte[0]);
        builder.redirectInput(new File("/dev/null"));builder.redirectErrorStream(true);builder.redirectOutput(ProcessBuilder.Redirect.appendTo(log));
        return builder.start();
    }
}
