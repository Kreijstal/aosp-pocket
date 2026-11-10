// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketapi32guesttest;

import android.app.Activity;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.os.Bundle;
import android.os.Process;
import android.provider.Settings;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import android.widget.Button;
import android.widget.EditText;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.KeyEvent;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.InputStream;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.UUID;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;
import javax.net.ssl.HttpsURLConnection;

/** Runs through the real installed-app lifecycle, with its ordinary app UID. */
public final class MainActivity extends Activity {
    private SharedPreferences state;
    private String nonce;
    private void save(SharedPreferences.Editor edit) {
        if(!edit.commit())throw new IllegalStateException("Cannot persist app state");
    }
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        state=getSharedPreferences("state",MODE_PRIVATE);nonce=getIntent().getStringExtra("nonce");
        if(nonce==null || !nonce.matches("[a-f0-9]{32}"))throw new IllegalArgumentException("Missing test nonce");
        if(android.os.Build.VERSION.SDK_INT!=32 || Process.myUid()<10000 || getApplicationInfo().uid!=Process.myUid())
            throw new AssertionError("Expected an ordinary installed API 32 app");
        try{Class.forName("android.media.Spatializer");}catch(Exception error){throw new AssertionError(error);}
        boolean fresh=!nonce.equals(state.getString("nonce",null));
        long processStarts=state.getLong("processStarts",0)+(state.getInt("pid",-1)!=Process.myPid()?1:0);
        SharedPreferences.Editor initial=state.edit().putString("nonce",nonce).putInt("pid",Process.myPid()).putInt("uid",Process.myUid())
            .putLong("starts",state.getLong("starts",0)+1).putLong("processStarts",processStarts)
            .putBoolean("resumed",false).putBoolean("drawn",false).putBoolean("checksDone",false).remove("error");
        if(fresh)initial.putInt("clicks",0).putString("typedText","").putInt("keyUps",0).putInt("lastKeyUp",-1);
        save(initial.putBoolean("textFocused",false));
        LinearLayout root=new LinearLayout(this);root.setOrientation(LinearLayout.VERTICAL);root.setPadding(24,48,24,24);
        root.setBackgroundColor(Color.rgb(230,242,255));
        TextView title=new TextView(this);title.setText("Android 12L app test");title.setTextSize(26);root.addView(title);
        android.view.View pattern=new android.view.View(this) {
            final Paint paint=new Paint();
            @Override protected void onDraw(Canvas canvas) {
                super.onDraw(canvas);
                for(int n=0;n<16;n++) {
                    int value=Integer.parseInt(nonce.substring(n*2,n*2+2),16);
                    paint.setColor(Color.rgb(value,value^0x5a,37+13*n));
                    canvas.drawRect(n*getWidth()/16,0,(n+1)*getWidth()/16,getHeight(),paint);
                }
            }
        };
        root.addView(pattern,new LinearLayout.LayoutParams(-1,32));
        Button button=new Button(this);button.setText("Tap to verify input");root.addView(button);
        button.setOnClickListener(view->{save(state.edit().putInt("clicks",state.getInt("clicks",0)+1));button.setText("Input received");});
        EditText editor=new EditText(this);editor.setHint("Keyboard check");editor.setSingleLine(true);
        editor.setInputType(android.text.InputType.TYPE_CLASS_TEXT|android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        // This fixture tests forwarded hardware keys independently of the guest IME.
        editor.setShowSoftInputOnFocus(false);editor.setText(state.getString("typedText",""));root.addView(editor);
        editor.setOnFocusChangeListener((v,focused)->save(state.edit().putBoolean("textFocused",focused)));
        editor.addTextChangedListener(new TextWatcher(){
            public void beforeTextChanged(CharSequence text,int start,int count,int after){}
            public void onTextChanged(CharSequence text,int start,int before,int count){save(state.edit().putString("typedText",text.toString()));}
            public void afterTextChanged(Editable text){}
        });
        root.setFocusableInTouchMode(true);setContentView(root);root.requestFocus();
        root.getViewTreeObserver().addOnGlobalLayoutListener(()->{
            int[] a=new int[2],b=new int[2],c=new int[2];pattern.getLocationOnScreen(a);button.getLocationOnScreen(b);editor.getLocationOnScreen(c);
            save(state.edit().putInt("patternLeft",a[0]).putInt("patternTop",a[1]).putInt("patternWidth",pattern.getWidth()).putInt("patternHeight",pattern.getHeight())
                .putInt("buttonX",b[0]+button.getWidth()/2).putInt("buttonY",b[1]+button.getHeight()/2)
                .putInt("editorX",c[0]+editor.getWidth()/2).putInt("editorY",c[1]+editor.getHeight()/2));
        });
        root.getViewTreeObserver().addOnDrawListener(()->{
            if(!state.getBoolean("drawn",false))root.post(()->save(state.edit().putBoolean("drawn",true)));
        });
        new Thread(()->{
            try {
                checkSettings();checkKey();checkNetwork();
            } catch(Throwable error) {
                android.util.Log.e("PocketApi32Guest","Application check failed",error);
                save(state.edit().putString("error",error.toString()));
            } finally {save(state.edit().putBoolean("checksDone",true));}
        },"InstalledApi32Checks").start();
    }
    @Override public void onResume(){super.onResume();save(state.edit().putBoolean("resumed",true));}
    @Override public void onPause(){save(state.edit().putBoolean("resumed",false));super.onPause();}
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        boolean handled=super.dispatchKeyEvent(event);
        if(event.getAction()==KeyEvent.ACTION_UP)save(state.edit().putInt("keyUps",state.getInt("keyUps",0)+1).putInt("lastKeyUp",event.getKeyCode()));
        return handled;
    }
    private void checkSettings()throws Exception {
        String value=Settings.Global.getString(getContentResolver(),Settings.Global.AIRPLANE_MODE_ON);
        try(Cursor cursor=getContentResolver().query(Settings.Global.getUriFor(Settings.Global.AIRPLANE_MODE_ON),new String[]{"name","value"},null,null,null)) {
            if(value==null || cursor==null || !cursor.moveToFirst() ||
                !Settings.Global.AIRPLANE_MODE_ON.equals(cursor.getString(0)) || !value.equals(cursor.getString(1)) || cursor.moveToNext())
                throw new AssertionError("Ordinary Settings read/query failed");
        }
        save(state.edit().putBoolean("settings",true));
    }
    private void checkKey()throws Exception {
        final String alias="pocket-api32-installed-state";
        KeyStore store=KeyStore.getInstance("AndroidKeyStore");store.load(null);
        String encoded=state.getString("encrypted",null);
        if(encoded==null) {
            if(store.containsAlias(alias))store.deleteEntry(alias);
            KeyGenerator generator=KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES,"AndroidKeyStore");
            generator.init(new KeyGenParameterSpec.Builder(alias,KeyProperties.PURPOSE_ENCRYPT|KeyProperties.PURPOSE_DECRYPT)
                .setKeySize(256).setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build());
            SecretKey key=generator.generateKey();if(key.getEncoded()!=null)throw new AssertionError("Exported AndroidKeyStore key");
            Cipher cipher=Cipher.getInstance("AES/GCM/NoPadding");cipher.init(Cipher.ENCRYPT_MODE,key);
            byte[] plain=UUID.randomUUID().toString().getBytes(StandardCharsets.UTF_8);
            save(state.edit().putString("encrypted",Base64.encodeToString(cipher.doFinal(plain),Base64.NO_WRAP))
                .putString("iv",Base64.encodeToString(cipher.getIV(),Base64.NO_WRAP))
                .putString("digest",Base64.encodeToString(MessageDigest.getInstance("SHA-256").digest(plain),Base64.NO_WRAP)));
            Arrays.fill(plain,(byte)0);
        }
        store=KeyStore.getInstance("AndroidKeyStore");store.load(null);
        SecretKey key=(SecretKey)store.getKey(alias,null);if(key==null || key.getEncoded()!=null)throw new AssertionError("Stored key unavailable");
        Cipher cipher=Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.DECRYPT_MODE,key,new GCMParameterSpec(128,Base64.decode(state.getString("iv",null),Base64.NO_WRAP)));
        byte[] plain=cipher.doFinal(Base64.decode(state.getString("encrypted",null),Base64.NO_WRAP));
        String digest=Base64.encodeToString(MessageDigest.getInstance("SHA-256").digest(plain),Base64.NO_WRAP);Arrays.fill(plain,(byte)0);
        if(!digest.equals(state.getString("digest",null)))throw new AssertionError("Private encrypted state changed");
        save(state.edit().putBoolean("keystore",true));
    }
    private void checkNetwork()throws Exception {
        HttpsURLConnection connection=(HttpsURLConnection)new URL("https://example.com/?pocket_api32_probe="+nonce).openConnection();
        connection.setConnectTimeout(15000);connection.setReadTimeout(15000);connection.setUseCaches(false);
        int bytes=0,status;
        try {
            status=connection.getResponseCode();
            if(status!=200 || connection.getServerCertificates().length==0)throw new AssertionError("HTTPS response "+status);
            try(InputStream input=connection.getInputStream()) {
                byte[] buffer=new byte[4096];int count;
                while(bytes<65536 && (count=input.read(buffer))!=-1)bytes+=count;
            }
            if(bytes==0)throw new AssertionError("Empty HTTPS response");
        } finally {connection.disconnect();}
        save(state.edit().putInt("httpStatus",status).putInt("httpBytes",bytes));
    }
}
