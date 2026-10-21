// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketattestationtest;

import android.app.Activity;
import android.os.Bundle;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyInfo;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import android.widget.TextView;
import java.io.FileOutputStream;
import java.security.*;
import java.security.cert.Certificate;
import java.security.cert.X509Certificate;
import java.security.spec.ECGenParameterSpec;
import java.util.UUID;
import org.json.*;

/** Offline API probe. Temporary test keys only; no imported or existing keys. */
public final class MainActivity extends Activity {
    private TextView text;
    private static String b64(byte[] bytes){return Base64.encodeToString(bytes,Base64.NO_WRAP);}
    private static JSONObject error(Throwable error)throws JSONException {
        JSONObject result=new JSONObject();JSONArray causes=new JSONArray();
        for(Throwable e=error;e!=null;e=e.getCause()) {
            JSONObject cause=new JSONObject().put("class",e.getClass().getName()).put("message",String.valueOf(e.getMessage()));
            try{cause.put("errorCode",e.getClass().getMethod("getErrorCode").invoke(e));}catch(Exception unavailable){}
            causes.put(cause);
        }
        return result.put("causes",causes);
    }
    private static JSONObject test(boolean attest,boolean strongBox,byte[] challenge)throws Exception {
        JSONObject result=new JSONObject().put("attestationRequested",attest).put("strongBoxRequested",strongBox);
        String alias="pocket-attestation-test-"+UUID.randomUUID();KeyStore store=null;
        String stage="load_keystore";
        try {
            store=KeyStore.getInstance("AndroidKeyStore");store.load(null);
            stage="generate_key";
            KeyPairGenerator generator=KeyPairGenerator.getInstance("EC","AndroidKeyStore");
            KeyGenParameterSpec.Builder parameters=new KeyGenParameterSpec.Builder(alias,KeyProperties.PURPOSE_SIGN|KeyProperties.PURPOSE_VERIFY)
                .setAlgorithmParameterSpec(new ECGenParameterSpec("secp256r1")).setDigests(KeyProperties.DIGEST_SHA256)
                .setUserAuthenticationRequired(false);
            if(attest)parameters.setAttestationChallenge(challenge);
            if(strongBox)parameters.setIsStrongBoxBacked(true);
            generator.initialize(parameters.build());KeyPair pair=generator.generateKeyPair();
            result.put("keyGenerated",true);
            stage="inspect_key";
            KeyFactory factory=KeyFactory.getInstance("EC","AndroidKeyStore");
            KeyInfo info=factory.getKeySpec(pair.getPrivate(),KeyInfo.class);
            result.put("insideSecureHardware",info.isInsideSecureHardware());
            stage="sign_verify";
            Signature signer=Signature.getInstance("SHA256withECDSA");signer.initSign(pair.getPrivate());signer.update(challenge);
            byte[] signature=signer.sign();Signature verifier=Signature.getInstance("SHA256withECDSA");
            verifier.initVerify(pair.getPublic());verifier.update(challenge);result.put("signatureVerified",verifier.verify(signature));
            stage="certificate_chain";
            Certificate[] chain=store.getCertificateChain(alias);JSONArray certificates=new JSONArray();
            if(chain!=null)for(Certificate item:chain){
                X509Certificate cert=(X509Certificate)item;
                JSONObject entry=new JSONObject().put("der",b64(cert.getEncoded())).put("subject",cert.getSubjectX500Principal().getName())
                    .put("issuer",cert.getIssuerX500Principal().getName());
                byte[] extension=cert.getExtensionValue("1.3.6.1.4.1.11129.2.1.17");
                if(extension!=null)entry.put("attestationExtension",b64(extension));
                certificates.put(entry);
            }
            result.put("certificates",certificates).put("status","PASS");
        } catch(Throwable failure) {
            result.put("status","FAIL").put("stage",stage).put("error",error(failure));
        } finally {
            try{if(store!=null)store.deleteEntry(alias);result.put("temporaryKeyDeleted",true);}
            catch(Throwable failure){result.put("cleanupError",error(failure));}
        }
        return result;
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);text=new TextView(this);text.setText("Testing guest Keystore attestation offline…");setContentView(text);
        new Thread(()->{
            JSONObject report=new JSONObject();
            try {
                byte[] challenge=new byte[32];new SecureRandom().nextBytes(challenge);
                report.put("scope","ordinary guest application; local Keystore test, not Google Play Integrity")
                    .put("testVersion","0.2").put("uid",android.os.Process.myUid()).put("sdk",android.os.Build.VERSION.SDK_INT).put("challenge",b64(challenge));
                JSONArray providers=new JSONArray();for(Provider provider:Security.getProviders())providers.put(provider.getName());report.put("providers",providers);
                report.put("plain",test(false,false,challenge));
                report.put("attested",test(true,false,challenge));
                report.put("strongBox",test(true,true,challenge));
                if(Security.getProvider("AndroidKeyStore")==null){
                    JSONObject diagnostic=new JSONObject();report.put("explicitStockProviderDiagnostic",diagnostic);
                    try {
                        // Android 9 ZygoteInit normally calls this during preload.
                        // This diagnostic affects only this probe process, not the guest OS.
                        Class.forName("android.security.keystore.AndroidKeyStoreProvider").getMethod("install").invoke(null);
                        diagnostic.put("providerInstalled",Security.getProvider("AndroidKeyStore")!=null);
                        diagnostic.put("plain",test(false,false,challenge));
                        diagnostic.put("attested",test(true,false,challenge));
                        diagnostic.put("strongBox",test(true,true,challenge));
                    }catch(Throwable failure){diagnostic.put("initializationError",error(failure));}
                }
            } catch(Throwable failure){try{report.put("fatal",error(failure));}catch(JSONException ignored){}}
            final String json=report.toString();
            try(FileOutputStream out=openFileOutput("report.json",MODE_PRIVATE)){out.write(json.getBytes("UTF-8"));}
            catch(Exception failure){android.util.Log.e("PocketKeyAttest","Cannot save report",failure);}
            android.util.Log.i("PocketKeyAttest",json);
            runOnUiThread(()->text.setText(json));
        },"PocketKeyAttestation").start();
    }
}
