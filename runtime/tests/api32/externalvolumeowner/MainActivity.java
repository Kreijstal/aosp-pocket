// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketvolumeowner32;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import android.os.*;
import android.system.Os;
import android.system.OsConstants;
import android.system.ErrnoException;
import android.widget.TextView;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.Arrays;
import java.security.KeyStore;
import java.security.MessageDigest;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;
import org.json.*;

/** A second registered ordinary UID supplies real, independently verified canaries. */
public final class MainActivity extends Activity {
    private volatile String report="{\"validation\":\"RUNNING\"}";
    private String lastCommand="";
    private volatile FileDescriptor held;
    private String heldPath;
    private final Handler watcher=new Handler(Looper.getMainLooper());
    private final Runnable watch=new Runnable(){public void run(){command();watcher.postDelayed(this,1000);}};
    private static void check(boolean value,String why)throws IOException {if(!value)throw new IOException(why);}
    private synchronized void command() {
        try {
            String command=new String(Files.readAllBytes(new File("/tmp/pocket-volume-owner-action").toPath()),StandardCharsets.US_ASCII).trim();
            if(command.equals(lastCommand))return;
            String[] parts=command.split(" ");
            boolean key=parts.length==2 && parts[0].startsWith("key-");
            if(key)parts[0]=parts[0].substring(4);
            check(parts.length==2 && parts[1].matches("[a-f0-9]{32}")
                && Arrays.asList("seed","verify","cleanup","hold","unmounted","remounted").contains(parts[0]),"Invalid owner command");
            lastCommand=command;report="{\"validation\":\"RUNNING\"}";
            new Thread(()->proof(parts[0],parts[1],key),"PrivateStorageOwner").start();
        }catch(Exception absent){/* A retained task may be restored before a request exists. */}
    }
    private JSONObject keyCanary(String action,String nonce)throws Exception {
        String alias="pocket-volume-crash-"+nonce;
        KeyStore store=KeyStore.getInstance("AndroidKeyStore");store.load(null);
        SharedPreferences prefs=getSharedPreferences(alias,MODE_PRIVATE);
        byte[] plain=("Pocket Keystore canary "+nonce).getBytes(StandardCharsets.US_ASCII);
        if(action.equals("seed")) {
            check(!store.containsAlias(alias) && prefs.getAll().isEmpty(),"Previous key canary must be inspected");
            KeyGenerator generator=KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES,"AndroidKeyStore");
            generator.init(new KeyGenParameterSpec.Builder(alias,KeyProperties.PURPOSE_ENCRYPT|KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).setKeySize(256).build());
            SecretKey key=generator.generateKey();Cipher encrypt=Cipher.getInstance("AES/GCM/NoPadding");encrypt.init(Cipher.ENCRYPT_MODE,key);
            check(prefs.edit().putString("cipher",Base64.encodeToString(encrypt.doFinal(plain),Base64.NO_WRAP))
                .putString("iv",Base64.encodeToString(encrypt.getIV(),Base64.NO_WRAP)).commit(),"Cannot persist key canary");
        }
        check(store.containsAlias(alias),"Original Keystore key is missing");
        SecretKey key=(SecretKey)store.getKey(alias,null);check(key!=null && key.getEncoded()==null,"Key is missing or exportable");
        String saved=prefs.getString("cipher",null),iv=prefs.getString("iv",null);check(saved!=null && iv!=null,"Key canary preferences are missing");
        byte[] ciphertext=Base64.decode(saved,Base64.NO_WRAP);Cipher decrypt=Cipher.getInstance("AES/GCM/NoPadding");
        decrypt.init(Cipher.DECRYPT_MODE,key,new GCMParameterSpec(128,Base64.decode(iv,Base64.NO_WRAP)));
        check(Arrays.equals(plain,decrypt.doFinal(ciphertext)),"Original key cannot decrypt the saved ciphertext");
        StringBuilder digest=new StringBuilder();for(byte b:MessageDigest.getInstance("SHA-256").digest(ciphertext))digest.append(String.format(java.util.Locale.ROOT,"%02x",b&255));
        JSONObject result=new JSONObject().put("alias",alias).put("cipherSha256",digest.toString()).put("nonExportable",true).put("cryptoValid",true);
        if(action.equals("cleanup")) {
            store.deleteEntry(alias);check(!store.containsAlias(alias),"Own test key was not removed");
            check(prefs.edit().clear().commit(),"Own key canary preferences were not removed");result.put("removed",true);
        }
        return result;
    }
    private void proof(String action,String nonce,boolean key) {
        JSONObject result=new JSONObject();JSONArray files=new JSONArray();
        try {
            check(Build.VERSION.SDK_INT==32 && Os.getuid()>=10000,"Expected ordinary API32 app");
            result.put("sdk",Build.VERSION.SDK_INT).put("uid",Os.getuid()).put("package",getPackageName())
                .put("nonce",nonce).put("action",action).put("files",files).put("pid",Os.getpid());
            if(key)result.put("keyCanary",keyCanary(action,nonce));
            if(action.equals("unmounted")) {
                String state=Environment.getExternalStorageState();result.put("storageState",state);
                check(held==null,"Old descriptor owner survived volume unmount");result.put("newProcessNoHeldDescriptor",true);
                FileDescriptor opened=null;
                String path="/storage/emulated/0/Android/data/"+getPackageName()+"/files/pocket-canary-"+nonce;
                try{opened=Os.open(path,OsConstants.O_RDONLY|OsConstants.O_CLOEXEC,0);throw new IOException("New path still opens after unmount");}
                catch(ErrnoException expected){check(expected.errno==OsConstants.ENODEV || expected.errno==OsConstants.ENOTCONN || expected.errno==OsConstants.ENOENT,
                    "Unexpected unmounted path error: "+expected);result.put("newOpenErrno",expected.errno);}
                finally{if(opened!=null)Os.close(opened);}
                check(Environment.MEDIA_UNMOUNTED.equals(state),"Ordinary app did not observe unmounted storage: "+state);
                result.put("validation","PASS");return;
            }
            String state=Environment.getExternalStorageState();result.put("storageState",state);
            check(Environment.MEDIA_MOUNTED.equals(state),"Storage is not mounted: "+state);
            File[] roots={getExternalFilesDir(null),getObbDir()};String[] areas={"data","obb"};
            byte[] payload=("Pocket private canary "+nonce).getBytes(StandardCharsets.US_ASCII);
            for(int i=0;i<roots.length;i++) {
                File root=roots[i];check(root!=null,"Own private directory is missing");
                File file=new File(root,"pocket-canary-"+nonce);
                if(action.equals("cleanup")) {
                    check(Files.deleteIfExists(file.toPath()),"Canary missing before owner cleanup");
                    check(!file.exists(),"Owner cleanup left the canary");
                    files.put(new JSONObject().put("area",areas[i]).put("path",file.getPath()).put("removed",true));continue;
                }
                check(root.isDirectory(),"Own private directory is inaccessible");
                if(action.equals("seed"))Files.write(file.toPath(),payload,StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE);
                check(Arrays.equals(payload,Files.readAllBytes(file.toPath())),"Owner canary bytes differ");
                android.system.StructStat stat=Os.stat(file.getPath());
                check(stat.st_size==payload.length,"Owner canary size differs");
                files.put(new JSONObject().put("area",areas[i]).put("path",file.getPath()).put("bytes",stat.st_size)
                    .put("inode",stat.st_ino).put("device",stat.st_dev).put("ownReadVerified",true));
            }
            if(action.equals("hold")) {
                check(held==null,"A previous held descriptor remains");heldPath=files.getJSONObject(0).getString("path");
                held=Os.open(heldPath,OsConstants.O_RDONLY|OsConstants.O_CLOEXEC,0);
                check(Os.pread(held,new byte[1],0,1,0)==1,"Held descriptor did not initially read");result.put("heldFdOpen",true);
            }
            if(action.equals("remounted")) {
                check(held==null,"Remounted owner retained the old process descriptor");result.put("newProcessNoHeldDescriptor",true);
            }
            if(action.equals("cleanup") && held!=null){Os.close(held);held=null;}
            result.put("validation","PASS");
        }catch(Throwable error){try{result.put("validation","FAIL").put("error",error.toString());}catch(Exception ignored){}}
        finally{report=result.toString();runOnUiThread(()->((TextView)findViewById(android.R.id.text1)).setText(report));}
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);TextView view=new TextView(this);view.setId(android.R.id.text1);view.setText("Waiting for owner request");setContentView(view);watcher.post(watch);
    }
    @Override protected void onResume(){super.onResume();command();}
    @Override protected void onNewIntent(Intent intent){super.onNewIntent(intent);setIntent(intent);command();}
    @Override protected void onDestroy(){watcher.removeCallbacks(watch);super.onDestroy();}
    @Override public void dump(String prefix,FileDescriptor fd,PrintWriter writer,String[] args) {
        writer.println("POCKET_VOLUME_OWNER32="+android.util.Base64.encodeToString(report.getBytes(StandardCharsets.UTF_8),android.util.Base64.NO_WRAP));
    }
}
