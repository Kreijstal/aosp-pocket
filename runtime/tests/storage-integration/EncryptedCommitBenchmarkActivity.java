// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.app.Activity;
import android.os.Bundle;
import android.util.Base64;
import java.io.PrintWriter;
import org.json.JSONObject;
import dev.kreijstal.aosppocket.storage.EncryptedCommitBatchTest;

/** DUMP-only receipt; exercises its own fresh store, never a guest profile. */
public final class EncryptedCommitBenchmarkActivity extends Activity {
    private volatile String receipt="{}";
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        final String nonce=getIntent().getStringExtra("probe_run_id");
        if(nonce==null || !nonce.matches("[a-f0-9]{32}"))throw new IllegalArgumentException("Test nonce required");
        new Thread(()->{
            JSONObject result=new JSONObject();
            try {result=new JSONObject(EncryptedCommitBatchTest.run(getFilesDir().toPath()));}
            catch(Throwable error){try{result.put("validation","FAIL").put("error",error.toString());}catch(Exception ignored){}}
            try {result.put("runId",nonce).put("hostUid",android.os.Process.myUid())
                .put("hostPid",android.os.Process.myPid()).put("normalGuestProfileAccess",false);}
            catch(Exception ignored){}
            receipt=result.toString();
        },"PocketCommitBenchmark").start();
    }
    @Override public void dump(String prefix,java.io.FileDescriptor fd,PrintWriter writer,String[] args) {
        if(args.length==1 && args[0].equals("pocket-commit-benchmark"))
            writer.println("POCKET_COMMIT_BENCHMARK="+Base64.encodeToString(receipt.getBytes(java.nio.charset.StandardCharsets.UTF_8),Base64.NO_WRAP));
        else super.dump(prefix,fd,writer,args);
    }
}
