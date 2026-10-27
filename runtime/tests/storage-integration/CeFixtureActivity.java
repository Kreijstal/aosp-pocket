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
import dev.kreijstal.aosppocket.storage.*;
import org.json.JSONObject;
/** Isolated production-adapter test. Never accesses Manager's root or user data. */
public final class CeFixtureActivity extends Activity {
    private File root;
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
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);TextView text=new TextView(this);setContentView(text);text.setText("Testing credential-encrypted filesystem…");
        new Thread(()-> {
            JSONObject report=new JSONObject();
            try {
                concurrentSyncTransport();report.put("readDuringBlockedSync","PASS: real AppFuse bridge permits reads before fsync finishes");
                root=new File(getFilesDir(),"fixture");if(root.exists())remove(root.toPath());root.mkdirs();
                for(String path:getAssets().list("root/system/bin"))extract("system/bin/"+path);
                for(String path:getAssets().list("root/system/lib64"))extract("system/lib64/"+path);
                for(String path:getAssets().list("root/system/usr/icu"))extract("system/usr/icu/"+path);
                for(String path:new String[]{"system-data","dev/socket",".pocket-owner",".pocket-ce-dirs",".pocket-ce-locks"})new File(root,path).mkdirs();
                NativeLoader.prepare(root);
                byte[] secret=new byte[64];new java.security.SecureRandom().nextBytes(secret);
                try {
                    File source=new File(root,"system-data/user/0");source.mkdirs();Files.write(new File(source,"seed").toPath(),"migration-canary".getBytes("UTF-8"));
                    CeMigration.perform(root,secret);
                    if((Os.stat(source.getPath()).st_mode&0777)!=0 || !CeStorage.enabled(root))throw new IOException("CE migration did not seal the plaintext source root");
                    report.put("migration","verified ciphertext; plaintext source removed");
                    try(CeStorage storage=new CeStorage(root)) {
                        report.put("locked",run("locked"));storage.unlock(secret);report.put("native",run("exercise"));
                    }
                    try(CeStorage storage=new CeStorage(root)){storage.unlock(secret);report.put("reopened",run("verify"));}
                    try(CeStorage storage=new CeStorage(root)){storage.unlock(secret);storage.prepareAuth(new byte[0]);CeDefaultKey.fixate(root);}
                    try(CeStorage storage=new CeStorage(root)){storage.unlock(new byte[0]);run("verify");storage.prepareAuth(secret);CeDefaultKey.fixate(root);}
                    try(CeStorage storage=new CeStorage(root)) {
                        try{storage.unlock(new byte[0]);throw new IOException("Authless key survived restoring credential protection");}
                        catch(SecurityException expected){}
                        storage.unlock(secret);run("verify");
                    }
                    report.put("credentialRemovalAndRestoration","PASS: host-keystore-wrapped default slot removed after restoring credential");
                }finally{Arrays.fill(secret,(byte)0);}
                report.put("uid",android.os.Process.myUid()).put("validation","PASS");
            }catch(Throwable error){try{report.put("validation","FAIL").put("error",error.toString());}catch(Exception ignored){}}
            final String result=report.toString();
            try(FileOutputStream out=openFileOutput("report.json",MODE_PRIVATE)){out.write(result.getBytes("UTF-8"));}catch(Exception ignored){}
            runOnUiThread(()->text.setText(result));
        },"CeIntegrationTest").start();
    }
    private static void remove(Path p)throws IOException {
        if(!Files.isSymbolicLink(p) && Files.isDirectory(p))try{Os.chmod(p.toString(),0700);}catch(Exception error){throw new IOException(error);}
        if(!Files.isSymbolicLink(p) && Files.isDirectory(p))try(DirectoryStream<Path> children=Files.newDirectoryStream(p)){for(Path c:children)remove(c);}
        Files.delete(p);
    }
}
