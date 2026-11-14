// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import android.app.Activity;
import android.os.Bundle;
import android.system.Os;
import android.widget.TextView;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.security.MessageDigest;
import dev.kreijstal.aosppocket.storage.*;
import org.json.JSONObject;
/** Isolated production-adapter test. Never accesses Manager's root or user data. */
public final class CeFixtureActivity extends Activity {
    private File root;
    private volatile String finished;
    @Override public void dump(String prefix,FileDescriptor fd,PrintWriter out,String[] args) {
        String value=finished==null?"{}":finished;
        out.println("POCKET_CE_FIXTURE="+android.util.Base64.encodeToString(value.getBytes(java.nio.charset.StandardCharsets.UTF_8),android.util.Base64.NO_WRAP));
    }
    public static final class BlockingSyncCallbacks {
        final CountDownLatch entered=new CountDownLatch(1),release=new CountDownLatch(1);
        public long fuseSize(int id){return 4096;}
        public byte[] fuseRead(int id,long offset,int size){byte[] data=new byte[size];Arrays.fill(data,(byte)81);return data;}
        public int fuseWrite(int id,long offset,byte[] data){return data.length;}
        public void fuseOpened(int id){}
        public void fuseRelease(int id){}
        public void fuseSync()throws Exception{entered.countDown();release.await();}
    }
    private static void concurrentSyncTransport()throws Exception {
        BlockingSyncCallbacks cb=new BlockingSyncCallbacks();
        try(CeFuse transport=new CeFuse(cb);
            android.os.ParcelFileDescriptor writer=transport.open(10,android.os.ParcelFileDescriptor.MODE_READ_WRITE);
            android.os.ParcelFileDescriptor reader=transport.open(11,android.os.ParcelFileDescriptor.MODE_READ_ONLY)) {
            FutureTask<Void> flush=new FutureTask<>(()->{Os.fsync(writer.getFileDescriptor());return null;});
            Thread sync=new Thread(flush,"FixtureBlockedSync");sync.setDaemon(true);sync.start();
            try {
                if(!cb.entered.await(5,TimeUnit.SECONDS))throw new IOException("AppFuse fsync did not reach callback");
                FutureTask<Integer> read=new FutureTask<>(()->{byte[] data=new byte[1];int n=Os.pread(reader.getFileDescriptor(),data,0,1,0);return n==1?(int)data[0]:-1;});
                Thread request=new Thread(read,"FixtureConcurrentRead");request.setDaemon(true);request.start();
                if(read.get(3,TimeUnit.SECONDS)!=81)throw new IOException("Concurrent AppFuse read contents");
                if(flush.isDone())throw new IOException("AppFuse fsync returned before callback completed");
            }finally{cb.release.countDown();flush.get(5,TimeUnit.SECONDS);}
        }
    }
    private void extract(String path)throws Exception {
        File dest=new File(root,path);dest.getParentFile().mkdirs();
        try(InputStream in=getAssets().open("root/"+path);OutputStream out=new FileOutputStream(dest)) {
            byte[] bytes=new byte[65536];int n;while((n=in.read(bytes))!=-1)out.write(bytes,0,n);
        }
        Os.chmod(dest.getPath(),0755);
    }
    private String run(String mode)throws Exception {
        ProcessBuilder command=new ProcessBuilder(new File(root,"system/bin/ce-test").getPath(),mode);
        command.directory(root);command.redirectErrorStream(true);command.environment().put("POCKET_SYSCALL_BACKEND","native");
        command.environment().put("POCKET_LOADER_BOOT","1");
        command.environment().put("LD_LIBRARY_PATH",new File(root,"system/lib64").getPath());
        java.lang.Process p=command.start();ByteArrayOutputStream bytes=new ByteArrayOutputStream();byte[] data=new byte[4096];int n;
        while((n=p.getInputStream().read(data))!=-1){bytes.write(data,0,n);if(bytes.size()>65536)throw new IOException("CE test output bound");}
        int result=p.waitFor();String output=bytes.toString("UTF-8");if(result!=0)throw new IOException("Native CE test "+mode+" exit="+result+" "+output);return output;
    }
    private String seedCheckpointMigration(File source,boolean sourceHardlink)throws Exception {
        for(int i=0;i<96;i++)Files.write(new File(source,"checkpoint-seed-"+i).toPath(),("seed-"+i).getBytes("UTF-8"),StandardOpenOption.CREATE_NEW);
        File large=new File(source,"large-checkpoint-file");MessageDigest hash=MessageDigest.getInstance("SHA-256");
        byte[] buffer=new byte[65536];long offset=0,size=17L*1024*1024+13;
        try(OutputStream output=Files.newOutputStream(large.toPath(),StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE)) {
            while(offset<size) {
                int count=(int)Math.min(buffer.length,size-offset);
                for(int i=0;i<count;i++)buffer[i]=(byte)((offset+i)*31+7);
                output.write(buffer,0,count);hash.update(buffer,0,count);offset+=count;
            }
        }
        if(sourceHardlink)Files.createLink(new File(source,"large-checkpoint-hardlink").toPath(),large.toPath());
        return android.util.Base64.encodeToString(hash.digest(),android.util.Base64.NO_WRAP);
    }
    private JSONObject verifyCheckpointMigration(byte[] secret,String expected,boolean sourceHardlink)throws Exception {
        Path vault=new File(root,".pocket-ce-v1").toPath();
        try(EncryptedStore store=EncryptedStore.open(vault,secret)) {
            CeNamespace ns=new CeNamespace(store,false);int[] groups={};
            CeNamespace.Node file=ns.resolve("/data/user/0/large-checkpoint-file",0,0,groups,true);
            if(sourceHardlink) {
                CeNamespace.Node link=ns.resolve("/data/user/0/large-checkpoint-hardlink",0,0,groups,true);
                if(file.id!=link.id || file.links!=2)throw new IOException("Migration did not preserve the hardlink inode");
            }
            String name=CeNamespace.content(file.id);long size=store.size(name),offset=0;
            if(size!=17L*1024*1024+13)throw new IOException("Migrated large file size differs");
            MessageDigest hash=MessageDigest.getInstance("SHA-256");byte[] bytes=new byte[65536];
            while(offset<size) {
                int count=store.read(name,offset,bytes,0,(int)Math.min(bytes.length,size-offset));
                if(count<=0)throw new IOException("Short migrated large-file read");hash.update(bytes,0,count);offset+=count;
            }
            if(!android.util.Base64.encodeToString(hash.digest(),android.util.Base64.NO_WRAP).equals(expected))throw new IOException("Migration checkpoint content differs");
            long before=0,after=0;
            try(DirectoryStream<Path> paths=Files.newDirectoryStream(vault.resolve("objects"))){for(Path path:paths)before++;}
            store.collectGarbage();
            try(DirectoryStream<Path> paths=Files.newDirectoryStream(vault.resolve("objects"))){for(Path path:paths)after++;}
            if(before!=after)throw new IOException("Migration retained obsolete ciphertext generations");
            if(!sourceHardlink) {
                ns.link("/data/user/0/large-checkpoint-file","/data/user/0/large-checkpoint-hardlink",0,0,groups);
                ns.sync();store.collectGarbage();
            }
            CeNamespace.Node link=ns.resolve("/data/user/0/large-checkpoint-hardlink",0,0,groups,true);
            if(link.id!=file.id || file.links!=2)throw new IOException("Encrypted hardlink inode differs");
            return new JSONObject().put("bytesVerified",size).put("sourceHardlinkPresentBeforeMigration",sourceHardlink)
                .put("sourceHardlinkPreserved",sourceHardlink).put("logicalHardlinkVerified",true)
                .put("ciphertextObjects",after).put("obsoleteCiphertextObjects",before-after).put("sha256Base64",expected);
        }
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);TextView text=new TextView(this);setContentView(text);text.setText("Testing credential-encrypted filesystem…");
        final boolean api32=getIntent().getBooleanExtra("api32_layout",false);
        final boolean checkpoints=getIntent().getBooleanExtra("checkpoint_migration",false);
        final boolean sourceHardlink=getIntent().getBooleanExtra("source_hardlink",true);
        final String runId=getIntent().getStringExtra("probe_run_id");
        new Thread(()-> {
            JSONObject report=new JSONObject();
            try {
                concurrentSyncTransport();report.put("readDuringBlockedSync","PASS: real AppFuse bridge permits reads before fsync finishes");
                root=new File(getFilesDir(),"fixture");if(root.exists())throw new IOException("Preserved fixture already exists; use a fresh fixture package");
                if(!root.mkdir())throw new IOException("Cannot create fixture directory");
                for(String path:getAssets().list("root/system/bin"))extract("system/bin/"+path);
                for(String path:getAssets().list("root/system/lib64"))extract("system/lib64/"+path);
                for(String path:getAssets().list("root/system/usr/icu"))extract("system/usr/icu/"+path);
                for(String path:new String[]{"system-data","dev/socket",".pocket-owner",".pocket-ce-dirs",".pocket-ce-locks"})new File(root,path).mkdirs();
                if(api32)Files.write(new File(root,"system/build.prop").toPath(),"ro.build.version.sdk=32\n".getBytes("UTF-8"));
                NativeLoader.prepare(root);
                byte[] secret=new byte[64];new java.security.SecureRandom().nextBytes(secret);
                try {
                    File source=new File(root,"system-data/user/0");source.mkdirs();Files.write(new File(source,"seed").toPath(),"migration-canary".getBytes("UTF-8"));
                    File vendor=new File(root,"system-data/vendor_ce/0");
                    if(api32){vendor.mkdirs();Files.write(new File(vendor,"seed").toPath(),"vendor-migration-canary".getBytes("UTF-8"));}
                    String largeDigest=checkpoints?seedCheckpointMigration(source,sourceHardlink):null;
                    CeMigrationSpace.Plan plan=CeMigration.preflight(root);
                    report.put("spacePreflight",new JSONObject().put("plaintextBytes",plan.plaintextBytes).put("requiredBytes",plan.requiredBytes).put("dataChunks",plan.dataChunks));
                    if(checkpoints && (plan.plaintextBytes<17L*1024*1024 || plan.plaintextBytes>18L*1024*1024))throw new IOException("Preflight counted a hardlink twice");
                    CeMigration.perform(root,secret);
                    if((Os.stat(source.getPath()).st_mode&0777)!=0 || !CeStorage.enabled(root))throw new IOException("CE migration did not seal the plaintext source root");
                    report.put("migration","verified ciphertext; plaintext source removed");
                    if(checkpoints)report.put("checkpointMigration",verifyCheckpointMigration(secret,largeDigest,sourceHardlink));
                    try(CeStorage storage=new CeStorage(root)) {
                        report.put("locked",run("locked"));if(api32)report.put("vendorLocked",run("vendor-locked"));
                        storage.unlock(secret);report.put("metadata",run("metadata"));
                        report.put("procFdReopen",run("proc-reopen"));
                        report.put("removedDirectory",run("removed-dir"));
                        report.put("native",run("exercise"));if(api32)report.put("vendorNative",run("vendor-exercise"));
                    }
                    try(CeStorage storage=new CeStorage(root)){storage.unlock(secret);report.put("metadataReopened",run("metadata-verify"));report.put("reopened",run("verify"));if(api32)report.put("vendorReopened",run("vendor-verify"));}
                    try(CeStorage storage=new CeStorage(root)){storage.unlock(secret);storage.prepareAuth(new byte[0]);CeDefaultKey.fixate(root);}
                    try(CeStorage storage=new CeStorage(root)){storage.unlock(new byte[0]);run("verify");storage.prepareAuth(secret);CeDefaultKey.fixate(root);}
                    try(CeStorage storage=new CeStorage(root)) {
                        try{storage.unlock(new byte[0]);throw new IOException("Authless key survived restoring credential protection");}
                        catch(SecurityException expected){}
                        storage.unlock(secret);run("verify");
                    }
                    report.put("credentialRemovalAndRestoration","PASS: host-keystore-wrapped default slot removed after restoring credential");
                    if(api32) {
                        Os.chmod(vendor.getPath(),0700);
                        try(DirectoryStream<Path> paths=Files.newDirectoryStream(vendor.toPath())){if(paths.iterator().hasNext())throw new IOException("Plaintext vendor CE directory is not empty");}
                        finally{Os.chmod(vendor.getPath(),0000);}
                        report.put("vendorPlaintextRemoved",true).put("ceRoots",5).put("layoutMarker",new File(root,CeMigration.LAYOUT32).isFile());
                    }
                }finally{Arrays.fill(secret,(byte)0);}
                report.put("uid",android.os.Process.myUid()).put("validation","PASS");
            }catch(Throwable error){try{report.put("validation","FAIL").put("error",error.toString());}catch(Exception ignored){}}
            try{report.put("runId",runId).put("api32Layout",api32).put("scope","Encrypted VFS, migration and native ARM64 file transport; not Android12L framework credential lifecycle");}catch(Exception ignored){}
            final String result=report.toString();finished=result;
            String reportId=runId!=null && runId.matches("[a-f0-9]{32}")?runId:UUID.randomUUID().toString();
            try{Files.write(new File(getFilesDir(),"report-"+reportId+".json").toPath(),result.getBytes("UTF-8"),StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE);}catch(Exception ignored){}
            runOnUiThread(()->text.setText(result));
        },"CeIntegrationTest").start();
    }
}
