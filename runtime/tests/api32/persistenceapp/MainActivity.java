// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketpersist32;

import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.os.*;
import android.os.Process;
import android.security.keystore.*;
import android.text.*;
import android.util.Base64;
import android.view.View;
import android.widget.*;
import java.nio.charset.StandardCharsets;
import java.security.*;
import java.util.*;
import javax.crypto.*;
import javax.crypto.spec.GCMParameterSpec;
import org.json.JSONObject;

/** Ordinary SDK32 app: private preferences and a real non-exportable AndroidKeyStore key. */
public final class MainActivity extends Activity {
    private SharedPreferences state;
    private Button button;
    private EditText editor;
    private TextView status;
    private boolean ready,drawn;
    private String token,digest,error;
    private final String activityId=UUID.randomUUID().toString().replace("-","");
    private static void check(boolean value,String message)throws Exception{if(!value)throw new Exception(message);}
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        if(Build.VERSION.SDK_INT!=32 || getApplicationInfo().minSdkVersion!=32 || Process.myUid()<10000)
            throw new IllegalStateException("Expected ordinary installed SDK32 app");
        if(getAttributionSource().getUid()!=Process.myUid())throw new IllegalStateException("Invalid API31 attribution source");
        state=getSharedPreferences("private-state",MODE_PRIVATE);token=state.getString("token",null);
        if(token==null)token=UUID.randomUUID().toString().replace("-","");
        if(!state.edit().putString("token",token).putLong("launches",state.getLong("launches",0)+1).commit())throw new IllegalStateException("Cannot persist launch");
        LinearLayout root=new LinearLayout(this){
            @Override protected void onDraw(Canvas canvas){super.onDraw(canvas);if(!drawn){drawn=true;post(()->emit());}}
        };
        root.setWillNotDraw(false);root.setOrientation(LinearLayout.VERTICAL);root.setBackgroundColor(0xffeaf5f2);
        root.setFocusableInTouchMode(true);
        TextView title=new TextView(this);title.setText("Android 12L private storage\nSDK "+Build.VERSION.SDK_INT+" · UID "+Process.myUid());title.setTextSize(20);root.addView(title);
        status=new TextView(this);status.setLines(2);status.setText("Checking AndroidKeyStore…");root.addView(status);
        button=new Button(this);button.setText("Persistent taps: "+state.getInt("taps",0));button.setEnabled(false);
        button.setOnClickListener(v->{int taps=state.getInt("taps",0)+1;if(!state.edit().putInt("taps",taps).commit())throw new IllegalStateException("Cannot persist tap");button.setText("Persistent taps: "+taps);emit();});
        root.addView(button);editor=new EditText(this);editor.setSingleLine(true);editor.setSelectAllOnFocus(true);editor.setText(state.getString("text",""));root.addView(editor);
        editor.addTextChangedListener(new TextWatcher(){
            public void beforeTextChanged(CharSequence s,int start,int count,int after){}
            public void onTextChanged(CharSequence s,int start,int before,int count){if(!state.edit().putString("text",s.toString()).commit())throw new IllegalStateException("Cannot persist text");emit();}
            public void afterTextChanged(Editable value){}
        });
        setContentView(root);root.requestFocus();
        new Thread(()->{
            try {
                KeyStore store=KeyStore.getInstance("AndroidKeyStore");store.load(null);
                String alias="pocket-persist32-aes";
                if(!store.containsAlias(alias)) {
                    KeyGenerator generator=KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES,"AndroidKeyStore");
                    generator.init(new KeyGenParameterSpec.Builder(alias,KeyProperties.PURPOSE_ENCRYPT|KeyProperties.PURPOSE_DECRYPT)
                        .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).setKeySize(256).build());
                    generator.generateKey();
                }
                SecretKey key=(SecretKey)store.getKey(alias,null);check(key!=null && key.getEncoded()==null,"Key is missing or exportable");
                byte[] plain=("private:"+token).getBytes(StandardCharsets.UTF_8);String savedCipher=state.getString("cipher",null);
                if(savedCipher==null) {
                    Cipher encrypt=Cipher.getInstance("AES/GCM/NoPadding");encrypt.init(Cipher.ENCRYPT_MODE,key);
                    check(state.edit().putString("cipher",Base64.encodeToString(encrypt.doFinal(plain),Base64.NO_WRAP))
                        .putString("iv",Base64.encodeToString(encrypt.getIV(),Base64.NO_WRAP)).commit(),"Cannot persist ciphertext");
                    savedCipher=state.getString("cipher",null);
                }
                byte[] cipher=Base64.decode(savedCipher,Base64.NO_WRAP);Cipher decrypt=Cipher.getInstance("AES/GCM/NoPadding");
                decrypt.init(Cipher.DECRYPT_MODE,key,new GCMParameterSpec(128,Base64.decode(state.getString("iv",""),Base64.NO_WRAP)));
                check(Arrays.equals(plain,decrypt.doFinal(cipher)),"Persisted ciphertext/key mismatch");
                byte[] sum=MessageDigest.getInstance("SHA-256").digest(cipher);StringBuilder hex=new StringBuilder();for(byte b:sum)hex.append(String.format(Locale.ROOT,"%02x",b&255));digest=hex.toString();
                // A real newer-than-Android10 package-manager call, not a version string check.
                check(getPackageManager().getInstallSourceInfo(getPackageName())!=null,"No install source information");
                runOnUiThread(()->{ready=true;status.setText("Private data and non-exportable key verified");button.setEnabled(true);button.post(()->emit());});
            }catch(Throwable failure){runOnUiThread(()->{error=failure.toString();status.setText(error);emit();});}
        },"PersistenceKeyStore").start();
    }
    @Override public void onResume(){super.onResume();if(state!=null){if(!state.edit().putLong("resumes",state.getLong("resumes",0)+1).commit())throw new IllegalStateException("Cannot persist resume");emit();}}
    private void emit() {
        if(state==null || button==null || editor==null || (!ready && error==null) || !drawn)return;
        try {
            int[] b=new int[2],e=new int[2];button.getLocationOnScreen(b);editor.getLocationOnScreen(e);
            JSONObject result=new JSONObject().put("sdk",Build.VERSION.SDK_INT).put("uid",Process.myUid()).put("pid",Process.myPid())
                .put("activityId",activityId).put("token",token).put("launches",state.getLong("launches",0)).put("resumes",state.getLong("resumes",0))
                .put("taps",state.getInt("taps",0)).put("text",state.getString("text","")).put("cryptoValid",ready).put("drawn",drawn)
                .put("cipherDigest",digest).put("error",error).put("buttonX",b[0]+button.getWidth()/2).put("buttonY",b[1]+button.getHeight()/2)
                .put("editorX",e[0]+editor.getWidth()/2).put("editorY",e[1]+editor.getHeight()/2);
            android.util.Log.i("PocketPersist32","STATE "+result);
        }catch(Exception failure){throw new IllegalStateException(failure);}
    }
}
