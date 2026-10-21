// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.content.*;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;
import dev.kreijstal.aosppocket.bridge.DisplayGeometry;

/** The pinned Android 9 image with syscall translation and unchanged Bionic. */
final class DirectImage {
    static boolean bundled(Context context) {
        try(InputStream in=context.getAssets().open("api28-rootfs/system/build.prop.payload")){return in.read()!=-1;}
        catch(IOException missing){return false;}
    }
    static File root(Context context) {
        return new File(context.getFilesDir(),"api28-rootfs");
    }
    /** Owner recovery remains available even when the experimental backend cannot boot. */
    static boolean disableAttestation(Context context) throws IOException {
        File image=root(context);
        File config=new File(image,"data/misc/keystore/pocket-teesim.json");
        if(!config.exists())return false;
        if(!config.getParentFile().getCanonicalPath().startsWith(image.getCanonicalPath()+"/"))
            throw new IOException("Attestation configuration escapes the guest image");
        File saved=new File(config.getParentFile(),"pocket-teesim.disabled-"+System.nanoTime()+".json");
        Files.move(config.toPath(),saved.toPath(),StandardCopyOption.ATOMIC_MOVE);
        return true;
    }
    /**
     * Processes left by an earlier session or the legacy direct backend.
     * The syscall backend ties its tracer to the app and kills tracees if that
     * tracer dies. Finding the tracer here also lets cleanup stop descendants.
     */
    static List<Integer> leftovers(File root) {
        List<Integer> found=new ArrayList<>();
        String prefix=root.getPath()+"/";
        File[] entries=new File("/proc").listFiles();
        if(entries==null)return found;
        for(File entry:entries) {
            if(!entry.getName().matches("\\d+"))continue;
            try {
                if(Os.readlink(new File(entry,"exe").getPath()).startsWith(prefix))found.add(Integer.parseInt(entry.getName()));
            } catch(ErrnoException | RuntimeException exited) { /* Exited, or owned by another UID. */ }
        }
        return found;
    }
    /** Returns how many leftover processes were found; throws if any survive a forced kill. */
    static int stopLeftovers(File root) throws IOException, InterruptedException {
        int found=0;
        for(int attempt=0;attempt<40;attempt++) {
            List<Integer> pids=leftovers(root);
            if(pids.isEmpty())return found;
            found=Math.max(found,pids.size());
            for(int pid:pids) {
                try {Os.kill(pid,attempt<20?OsConstants.SIGTERM:OsConstants.SIGKILL);}
                catch(ErrnoException gone) { /* Already exited. */ }
            }
            Thread.sleep(200);
        }
        throw new IOException("Leftover Android 9 processes did not exit: "+leftovers(root));
    }
    static void clearGeneratedLinks(File root) throws IOException {
        String[][] links={{"system/bin/getprop","/system/etc/pocket-magisk/getprop"},
                          {"system/bin/setprop","/system/bin/toybox"},
                          {"system/bin/magisk","/sbin/magisk"},
                          {"system/bin/su","/sbin/magisk"},
                          {"system/bin/resetprop","/sbin/magisk"}};
        for(String[] entry:links) {
            Path path=new File(root,entry[0]).toPath();
            if(Files.isSymbolicLink(path) && Files.readSymbolicLink(path).toString().equals(entry[1]))
                Files.delete(path);
        }
    }
    static File prepare(Context context,DisplayGeometry geometry,int refreshHz,GuestImage.Progress progress) throws Exception {
        File root=root(context);root.mkdirs();
        // Our optional runtime generates these links after extraction. Remove
        // only its known targets before validating/re-extracting bundled files.
        clearGeneratedLinks(root);
        // A previous boot's module view must not shadow the newly prepared
        // base image. Magisk publishes a fresh view before guest zygote starts.
        Files.deleteIfExists(new File(root,".pocket-module-view/ready").toPath());
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
        for(String name:new String[]{"system-data","dev/socket","dev/__properties__","dev/input","logs","tmp"})new File(root,name).mkdirs();
        File data=new File(root,"data");
        if(Files.isSymbolicLink(data.toPath())) {
            if(!Files.readSymbolicLink(data.toPath()).toString().equals("system-data"))
                throw new IOException("Unexpected Android 9 data link; refusing to replace it");
            Files.delete(data.toPath());
        } else if(data.exists() && (!data.isDirectory() || data.list()==null || data.list().length!=0))
            throw new IOException("Unexpected nonempty Android 9 data directory; refusing to replace it");
        // PRoot canonicalizes bind destinations. A symlink here would expose
        // /system-data as the canonical guest path instead of binding /data.
        if(!data.isDirectory() && !data.mkdir())throw new IOException("Cannot create guest data binding directory");
        for(File entry:new File(root,"dev/__properties__").listFiles())
            if(!entry.delete())throw new IOException("Cannot reset guest property area");
        File console=new File(root,"pocket-console");Files.deleteIfExists(console.toPath());
        Os.symlink("logs/manager-init.log",console.getPath());
        for(File executable:new File(root,"system/bin").listFiles())patchInterpreter(executable,"/system/bin/linker64");
        Files.write(new File(root,"system/etc/ld.config.txt").toPath(),("dir.pocket = /\n[pocket]\nnamespace.default.isolated = false\nnamespace.default.search.paths = /system/lib64:/vendor/lib64\nnamespace.default.permitted.paths = /\n").getBytes("UTF-8"));
        // This owned runtime payload is retired; installed user modules/data
        // remain separate. No Bionic-patching library is loaded by this backend.
        Files.deleteIfExists(new File(root,"system/lib64/libpocket_direct_boot.so").toPath());
        new File(root.getParentFile(),"syscall-tmp").mkdirs();
        Os.symlink("/system/etc/pocket-magisk/getprop",new File(root,"system/bin/getprop").getPath());
        Os.symlink("/system/bin/toybox",new File(root,"system/bin/setprop").getPath());
        GuestBinder.preparePlatform(context,root);
        File properties=new File(root,"system/build.prop"),temporary=new File(properties.getPath()+".display-new");
        String original=new String(Files.readAllBytes(properties.toPath()),"UTF-8");
        // Like VMOS, the guest language follows the host locale.
        String projected=property(geometry.properties(original,refreshHz),"ro.product.locale",Locale.getDefault().toLanguageTag());
        Files.write(temporary.toPath(),projected.getBytes("UTF-8"));
        Files.move(temporary.toPath(),properties.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
        return root;
    }
    private static String property(String source,String key,String value) {
        java.util.regex.Pattern pattern=java.util.regex.Pattern.compile("(?m)^"+java.util.regex.Pattern.quote(key)+"=.*$");
        String line=key+"="+value;
        if(pattern.matcher(source).find())return pattern.matcher(source).replaceAll(java.util.regex.Matcher.quoteReplacement(line));
        return source+(source.endsWith("\n")?"":"\n")+line+"\n";
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
        ProcessBuilder builder=new ProcessBuilder(SyscallBackend.command(root,"/",Arrays.asList(
            "/system/bin/pocket-init-api28","/system/etc/init/init.syscall.rc"))).directory(root);
        builder.environment().clear();
        builder.environment().putAll(SyscallBackend.environment(root,android.os.Process.myPid()));
        File log=new File(root,"logs/manager-init.log");Files.write(log.toPath(),new byte[0]);
        builder.redirectInput(new File("/dev/null"));builder.redirectErrorStream(true);builder.redirectOutput(ProcessBuilder.Redirect.appendTo(log));
        return builder.start();
    }
    /** Owner-authorized syscall-translated shell; no host kernel privileges. */
    static Process admin(File root,String cwd,List<String> argv) throws IOException {
        ProcessBuilder builder=new ProcessBuilder(SyscallBackend.command(root,cwd,argv)).directory(root);
        builder.environment().clear();
        builder.environment().putAll(SyscallBackend.environment(root,android.os.Process.myPid()));
        return builder.start();
    }
}
