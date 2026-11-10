// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketvolume32;

import android.app.Activity;
import android.content.ContentValues;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.*;
import android.provider.MediaStore;
import android.system.Os;
import android.system.OsConstants;
import android.system.ErrnoException;
import android.widget.TextView;
import java.io.*;
import java.nio.file.Files;
import java.util.*;
import org.json.*;

/** Registered ordinary app, using the production mounted volume and provider. */
public final class MainActivity extends Activity {
    private volatile String report="{\"validation\":\"RUNNING\"}";
    private String lastRequest="";
    private int iterations;
    private static final int O_PATH=0x200000;
    private static final int O_DIRECTORY=0x10000;
    private static final String OWNER="dev.kreijstal.pocketvolumeowner32";
    private static final byte[] PNG=android.util.Base64.decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aX1cAAAAASUVORK5CYII=",android.util.Base64.DEFAULT);
    private static void check(boolean value,String why)throws IOException {if(!value)throw new IOException(why);}
    private String data(Uri uri)throws Exception {
        try(Cursor cursor=getContentResolver().query(uri,new String[]{MediaStore.MediaColumns.DATA,MediaStore.MediaColumns.OWNER_PACKAGE_NAME},null,null,null)) {
            check(cursor!=null && cursor.moveToFirst(),"MediaStore row missing");
            check(getPackageName().equals(cursor.getString(1)),"MediaStore row has wrong owner");return cursor.getString(0);
        }
    }
    private int directRows(Uri collection,String name)throws Exception {
        try(Cursor cursor=getContentResolver().query(collection,new String[]{"_id"},MediaStore.MediaColumns.DISPLAY_NAME+"=?",new String[]{name},null)) {
            check(cursor!=null,"Direct-file query returned null");return cursor.getCount();
        }
    }
    private static void denied(String path,int flags,JSONArray checks)throws Exception {
        FileDescriptor fd=null;
        try {
            fd=Os.open(path,flags|OsConstants.O_CLOEXEC,0);
            throw new IOException("Another UID's private path opened: "+path+" flags="+flags);
        }catch(ErrnoException expected) {
            check(expected.errno==OsConstants.EACCES || expected.errno==OsConstants.ENOENT || expected.errno==OsConstants.EPERM,
                "Unexpected private-path open error: "+expected);
            checks.put(new JSONObject().put("path",path).put("operation","open").put("flags",flags).put("errno",expected.errno));
        }finally{if(fd!=null)Os.close(fd);}
    }
    private void crossUidPrivateDenied(String nonce,JSONObject result)throws Exception {
        int ownerUid=getPackageManager().getPackageInfo(OWNER,0).applicationInfo.uid;
        check(ownerUid>=10000 && ownerUid!=Os.getuid(),"Canary owner is not a different registered UID");
        JSONArray checks=new JSONArray();result.put("privateDenials",checks).put("privateOwnerUid",ownerUid);
        for(String area:new String[]{"data","obb"}) {
            String relative="/Android/"+area+"/"+OWNER+(area.equals("data")?"/files":"");
            for(String prefix:new String[]{"/storage/emulated/0","/sdcard","/data/media/0","/system-data/media/0"}) {
                String directory=prefix+relative,path=directory+"/pocket-canary-"+nonce;
                denied(path,OsConstants.O_RDONLY,checks);denied(path,O_PATH,checks);
                denied(directory,OsConstants.O_RDONLY|O_DIRECTORY,checks);denied(directory,O_PATH|O_DIRECTORY,checks);
                try{Os.stat(path);throw new IOException("Another UID's private stat succeeded: "+path);}
                catch(ErrnoException expected) {
                    check(expected.errno==OsConstants.EACCES || expected.errno==OsConstants.ENOENT || expected.errno==OsConstants.EPERM,
                        "Unexpected private-path stat error: "+expected);
                    checks.put(new JSONObject().put("path",path).put("operation","stat").put("errno",expected.errno));
                }
            }
        }
        check(checks.length()==40,"Incomplete private-directory denial matrix");
    }
    private void proof(String nonce,String phase,int iteration) {
        JSONObject result=new JSONObject();JSONArray passed=new JSONArray();
        Uri media=null;File direct=null,privateFile=null,parent=null;FileDescriptor held=null;
        try {
            check(Build.VERSION.SDK_INT==32 && Os.getuid()>=10000,"Expected ordinary API32 app");
            result.put("sdk",Build.VERSION.SDK_INT).put("uid",Os.getuid()).put("package",getPackageName()).put("nonce",nonce).put("passed",passed)
                .put("phase",phase).put("iteration",iteration).put("pid",Os.getpid());
            check(Environment.MEDIA_MOUNTED.equals(Environment.getExternalStorageState()),"External storage is not mounted");passed.put("mounted");
            Uri collection=MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY);
            try(Cursor cursor=getContentResolver().query(collection,new String[]{"_id"},"1=0",null,null)) {check(cursor!=null && cursor.getCount()==0,"Registered app external_primary query failed");}passed.put("registeredMediaStoreQuery");
            ContentValues values=new ContentValues();values.put(MediaStore.MediaColumns.DISPLAY_NAME,"content-"+nonce+".png");values.put(MediaStore.MediaColumns.MIME_TYPE,"image/png");
            values.put(MediaStore.MediaColumns.RELATIVE_PATH,"Pictures/Pocket12L/"+nonce+"/");values.put(MediaStore.MediaColumns.IS_PENDING,1);
            media=getContentResolver().insert(collection,values);check(media!=null,"MediaStore insert returned null");
            try(OutputStream stream=getContentResolver().openOutputStream(media,"w")){check(stream!=null,"Content stream missing");stream.write(PNG);}
            String original=data(media);File file=new File(original);parent=file.getParentFile();
            check(original.startsWith(Environment.getExternalStorageDirectory().getPath()+"/Pictures/Pocket12L/"+nonce+"/"),"Provider returned unexpected path");
            check(Arrays.equals(PNG,Files.readAllBytes(file.toPath())),"Content write differs through ordinary app path");passed.put("contentToPath");
            check(file.getCanonicalPath().equals(original),"Bionic canonical path differs");
            held=Os.open(original,O_PATH|OsConstants.O_CLOEXEC,0);
            check(Os.fstat(held).st_ino==Os.stat(original).st_ino && Os.fstat(held).st_dev==Os.stat(original).st_dev,"Provider path/FD metadata differs");passed.put("canonicalPathAndFd");
            values.clear();values.put(MediaStore.MediaColumns.IS_PENDING,0);check(getContentResolver().update(media,values,null,null)==1,"MediaStore publish failed");
            values.clear();values.put(MediaStore.MediaColumns.DISPLAY_NAME,"renamed-"+nonce+".png");check(getContentResolver().update(media,values,null,null)==1,"MediaStore rename failed");
            String renamed=data(media);check(!original.equals(renamed) && !file.exists() && Arrays.equals(PNG,Files.readAllBytes(new File(renamed).toPath())),"Renamed provider path is incoherent");
            check(Os.fstat(held).st_ino==Os.stat(renamed).st_ino,"Held path handle lost provider rename");Os.close(held);held=null;passed.put("providerRenameCoherence");
            direct=new File(parent,"direct-"+nonce+".png");check(!direct.exists(),"Direct test path already exists");
            FileDescriptor output=Os.open(direct.getPath(),OsConstants.O_CREAT|OsConstants.O_EXCL|OsConstants.O_RDWR|OsConstants.O_CLOEXEC,0600);
            try{check(Os.write(output,PNG,0,PNG.length)==PNG.length,"Short direct write");Os.fsync(output);}finally{Os.close(output);}
            check(Arrays.equals(PNG,Files.readAllBytes(direct.toPath())) && directRows(collection,direct.getName())==1,"Direct FUSE creation is absent from MediaStore");passed.put("fuseCreateAndMediaStore");
            Files.delete(direct.toPath());check(directRows(collection,direct.getName())==0,"FUSE unlink left a MediaStore row");direct=null;passed.put("fuseUnlinkCoherence");
            File privateRoot=getExternalFilesDir(null);check(privateRoot!=null && privateRoot.isDirectory(),"App external-files directory missing");
            privateFile=new File(privateRoot,"pocket-"+nonce);Files.write(privateFile.toPath(),PNG);check(Arrays.equals(PNG,Files.readAllBytes(privateFile.toPath())),"Own app-private external data differs");passed.put("ownExternalFiles");
            crossUidPrivateDenied(nonce,result);passed.put("crossUidPrivateDenied");
            JSONObject namespace=NamespaceProof.run(this,nonce);result.put("namespace",namespace);
            check("PASS".equals(namespace.getString("validation")) && namespace.getBoolean("cleanupComplete"),"Namespace syscall proof failed: "+namespace);
            passed.put("namespaceSyscalls");
            result.put("validation","PASS");
        }catch(Throwable error){try{result.put("validation","FAIL").put("error",error.toString());}catch(Exception ignored){}}
        finally {
            try {
                if(held!=null)Os.close(held);
                if(direct!=null)Files.deleteIfExists(direct.toPath());
                if(privateFile!=null)Files.deleteIfExists(privateFile.toPath());
                if(media!=null)check(getContentResolver().delete(media,null,null)==1,"Owned MediaStore cleanup failed");
                if(parent!=null && parent.isDirectory())check(parent.delete(),"Owned nonce directory cleanup failed");
                result.put("cleanupComplete",true);
            }catch(Exception error){try{result.put("validation","FAIL").put("cleanupError",error.toString());}catch(Exception ignored){}}
        }
        report=result.toString();runOnUiThread(()->((TextView)findViewById(android.R.id.text1)).setText(report));
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);TextView view=new TextView(this);view.setId(android.R.id.text1);view.setText("Waiting for a storage test request");setContentView(view);request();
    }
    private synchronized void request() {
        try {
            String nonce=new String(Files.readAllBytes(new File("/tmp/pocket-volume-app-run").toPath()),"US-ASCII").trim();
            check(nonce.matches("[a-f0-9]{32}"),"Invalid storage proof request");
            File phaseFile=new File("/tmp/pocket-volume-app-phase");
            String phase="initial";
            if(phaseFile.isFile()) {
                String[] request=new String(Files.readAllBytes(phaseFile.toPath()),"US-ASCII").trim().split(" ");
                check(request.length==2 && request[1].equals(nonce),"Phase request belongs to another run");phase=request[0];
            }
            check(phase.equals("initial") || phase.equals("remounted"),"Invalid workload phase");
            String id=nonce+":"+phase;if(lastRequest.equals(id))return;lastRequest=id;int iteration=++iterations;
            report="{\"validation\":\"RUNNING\"}";((TextView)findViewById(android.R.id.text1)).setText("Testing production mounted storage…");
            final String selectedPhase=phase;new Thread(()->proof(nonce,selectedPhase,iteration),"MountedStorageProof").start();
        }catch(Exception absent){/* Retained task restoration precedes the owner's request. */}
    }
    @Override protected void onResume(){super.onResume();request();}
    @Override protected void onNewIntent(Intent intent){super.onNewIntent(intent);setIntent(intent);request();}
    @Override public void dump(String prefix,FileDescriptor fd,PrintWriter writer,String[] args){writer.println("POCKET_VOLUME_APP32="+android.util.Base64.encodeToString(report.getBytes(java.nio.charset.StandardCharsets.UTF_8),android.util.Base64.NO_WRAP));}
}
