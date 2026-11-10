// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guestbridge;

import android.content.Context;
import android.net.*;
import android.os.*;
import java.net.InetAddress;
import java.util.List;
import java.util.concurrent.*;
import org.json.*;

/** Network diagnostics from an ordinary AMS app with a running main Looper. */
final class DnsApi32 {
    private static final class Reply<T> implements DnsResolver.Callback<T> {
        final CompletableFuture<T> value=new CompletableFuture<>();int rcode=-1;
        public void onAnswer(T answer,int code){rcode=code;value.complete(answer);}
        public void onError(DnsResolver.DnsException error){value.completeExceptionally(error);}
    }
    static Bundle check(Context context) {
        Bundle result=new Bundle();
        CancellationSignal typedCancel=new CancellationSignal(),rawCancel=new CancellationSignal(),negativeCancel=new CancellationSignal();
        ExecutorService callbacks=Executors.newSingleThreadExecutor();
        try {
            if(android.os.Process.myUid()<10000)throw new IllegalStateException("Expected an ordinary application");
            Network network=context.getSystemService(ConnectivityManager.class).getActiveNetwork();
            if(network==null)throw new IllegalStateException("No guest default network");
            DnsResolver resolver=DnsResolver.getInstance();
            Reply<List<InetAddress>> typed=new Reply<>();Reply<byte[]> raw=new Reply<>(),negative=new Reply<>();
            int flags=DnsResolver.FLAG_NO_CACHE_LOOKUP|DnsResolver.FLAG_NO_CACHE_STORE;
            resolver.query(network,"example.com",DnsResolver.TYPE_A,flags,callbacks,typedCancel,typed);
            resolver.rawQuery(network,"example.com",DnsResolver.CLASS_IN,DnsResolver.TYPE_A,flags,callbacks,rawCancel,raw);
            resolver.rawQuery(network,"pocket-api32-dns-check.invalid",DnsResolver.CLASS_IN,DnsResolver.TYPE_A,flags,callbacks,negativeCancel,negative);
            long deadline=SystemClock.elapsedRealtime()+12000;
            List<InetAddress> addresses=typed.value.get(Math.max(1,deadline-SystemClock.elapsedRealtime()),TimeUnit.MILLISECONDS);
            byte[] answer=raw.value.get(Math.max(1,deadline-SystemClock.elapsedRealtime()),TimeUnit.MILLISECONDS);
            byte[] absent=negative.value.get(Math.max(1,deadline-SystemClock.elapsedRealtime()),TimeUnit.MILLISECONDS);
            if(typed.rcode!=0 || addresses.isEmpty() || raw.rcode!=0 || answer.length<12 || (answer[2]&128)==0
                    || (answer[3]&15)!=0 || ((answer[6]&255)<<8|(answer[7]&255))==0
                    || negative.rcode!=3 || absent.length<12 || (absent[3]&15)!=3)
                throw new IllegalStateException("DNS result differs from expected positive/negative answers");
            JSONArray ips=new JSONArray();for(InetAddress address:addresses)ips.put(address.getHostAddress());
            JSONObject report=new JSONObject().put("uid",android.os.Process.myUid()).put("network",network.toString())
                .put("addresses",ips).put("rawBytes",answer.length).put("rawRcode",raw.rcode)
                .put("negativeRcode",negative.rcode).put("publicAsyncApi",true);
            result.putString("result",report.toString());
        }catch(Exception error){result.putString("error",error.toString());}
        finally{
            // Each resolver request owns its cancellation listener. Let already
            // queued callbacks drain after closing all outstanding requests.
            typedCancel.cancel();rawCancel.cancel();negativeCancel.cancel();callbacks.shutdown();
        }
        return result;
    }
}
