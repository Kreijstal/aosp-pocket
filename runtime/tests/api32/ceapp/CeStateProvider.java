// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketce32test;
import android.content.*;
import android.database.Cursor;
import android.net.Uri;
import android.os.*;
import android.security.keystore.*;
import android.util.Base64;
import java.security.*;
import java.util.*;
import javax.crypto.*;
import javax.crypto.spec.GCMParameterSpec;
import org.json.*;

/** Ordinary installed app checks its own private CE data and non-exportable key. */
public final class CeStateProvider extends ContentProvider {
    @Override public boolean onCreate() {
        if(Build.VERSION.SDK_INT!=32 || android.os.Process.myUid()<10000 || getContext().getApplicationInfo().minSdkVersion!=32)
            throw new IllegalStateException("Ordinary SDK32 app required");
        return true;
    }
    @Override public Bundle call(String method,String arg,Bundle extras) {
        int uid=Binder.getCallingUid();if(uid!=0 && uid!=1000)throw new SecurityException("System observer required");
        long identity=Binder.clearCallingIdentity();Bundle reply=new Bundle();String errorStage="initialization";
        try {
            if(!method.equals("create") && !method.equals("verify"))throw new IllegalArgumentException("Unknown state operation");
            Context context=getContext();SharedPreferences prefs=context.getSharedPreferences("ce-state",Context.MODE_PRIVATE);
            KeyStore keys=KeyStore.getInstance("AndroidKeyStore");keys.load(null);String alias="pocket-ce32-aes";
            if(method.equals("create")) {
                if(prefs.contains("token") || keys.containsAlias(alias))throw new IllegalStateException("Fixture state already exists");
                String token=UUID.randomUUID().toString();KeyGenerator generator=KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES,"AndroidKeyStore");
                generator.init(new KeyGenParameterSpec.Builder(alias,KeyProperties.PURPOSE_ENCRYPT|KeyProperties.PURPOSE_DECRYPT)
                    .setKeySize(256).setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build());
                SecretKey key=generator.generateKey();if(key.getEncoded()!=null)throw new IllegalStateException("Key is exportable");
                Cipher encrypt=Cipher.getInstance("AES/GCM/NoPadding");encrypt.init(Cipher.ENCRYPT_MODE,key);
                byte[] ciphertext=encrypt.doFinal(token.getBytes("UTF-8"));
                if(!prefs.edit().putString("token",token).putString("iv",Base64.encodeToString(encrypt.getIV(),Base64.NO_WRAP))
                    .putString("cipher",Base64.encodeToString(ciphertext,Base64.NO_WRAP)).commit())throw new IllegalStateException("Private state commit failed");
            }
            errorStage="private-data";String token=prefs.getString("token",null);if(token==null)throw new IllegalStateException("Persisted private state is missing");
            errorStage="keystore-key";if(!keys.containsAlias(alias))throw new IllegalStateException("Persisted key is missing");
            errorStage="decrypt-persisted-state";
            SecretKey key=(SecretKey)keys.getKey(alias,null);if(key.getEncoded()!=null)throw new IllegalStateException("Key became exportable");
            byte[] bytes=Base64.decode(prefs.getString("cipher",""),Base64.NO_WRAP),iv=Base64.decode(prefs.getString("iv",""),Base64.NO_WRAP);
            Cipher decrypt=Cipher.getInstance("AES/GCM/NoPadding");decrypt.init(Cipher.DECRYPT_MODE,key,new GCMParameterSpec(128,iv));
            if(!Arrays.equals(token.getBytes("UTF-8"),decrypt.doFinal(bytes)))throw new IllegalStateException("Stored ciphertext/key mismatch");
            if(context.getAttributionSource().getUid()!=android.os.Process.myUid() || context.getPackageManager().getInstallSourceInfo(context.getPackageName())==null)
                throw new IllegalStateException("SDK31 app identity API failed");
            errorStage="canonical-private-path";String canonical=context.getDataDir().getCanonicalPath();
            if(!canonical.equals(context.getDataDir().getPath()))throw new IllegalStateException("Private path canonicalization changed");
            StringBuilder digest=new StringBuilder();for(byte b:MessageDigest.getInstance("SHA-256").digest(bytes))digest.append(String.format(Locale.ROOT,"%02x",b&255));
            JSONObject state=new JSONObject().put("sdk",Build.VERSION.SDK_INT).put("uid",android.os.Process.myUid()).put("pid",android.os.Process.myPid())
                .put("token",token).put("cipherDigest",digest.toString()).put("nonExportable",true).put("cryptoValid",true)
                .put("privatePath",context.getDataDir().getPath()+"/shared_prefs/ce-state.xml").put("canonicalDataPath",canonical);
            reply.putString("result",state.toString());
        }catch(Exception error){reply.putString("error",error.getClass().getSimpleName());reply.putString("errorStage",errorStage);}
        finally{Binder.restoreCallingIdentity(identity);}
        return reply;
    }
    @Override public Cursor query(Uri uri,String[] p,String s,String[] a,String o){throw new UnsupportedOperationException();}
    @Override public String getType(Uri uri){return "application/vnd.pocket.ce-test";}
    @Override public Uri insert(Uri uri,ContentValues v){throw new UnsupportedOperationException();}
    @Override public int delete(Uri uri,String s,String[] a){throw new UnsupportedOperationException();}
    @Override public int update(Uri uri,ContentValues v,String s,String[] a){throw new UnsupportedOperationException();}
}
