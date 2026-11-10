// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketmediasession32;
import android.app.Activity;
import android.content.*;
import android.content.pm.PackageManager;
import android.net.*;
import android.os.*;
import android.system.*;
import android.util.Log;
import android.widget.TextView;
import java.io.*;
import java.lang.reflect.*;
import java.nio.file.*;
import java.util.concurrent.*;
import org.json.*;

/** Binds the stock MediaProvider service from a registered guest app process. */
public final class MainActivity extends Activity {
    private static final String TAG="PocketMediaSession";
    private String inputRun;
    @Override public boolean onKeyUp(int key,android.view.KeyEvent event) {
        if(inputRun!=null && key==android.view.KeyEvent.KEYCODE_9) {
            Log.i(TAG,"POCKET_MEDIA_INPUT32_KEY run="+inputRun+" uid="+Os.getuid());return true;
        }
        return super.onKeyUp(key,event);
    }
    private JSONObject bridgeNetwork()throws Exception {
        ConnectivityManager cm=getSystemService(ConnectivityManager.class);Network network=null;
        long deadline=SystemClock.elapsedRealtime()+45000;
        while(SystemClock.elapsedRealtime()<deadline) {
            network=cm.getActiveNetwork();NetworkCapabilities caps=network==null?null:cm.getNetworkCapabilities(network);
            if(caps!=null && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED))break;
            network=null;Thread.sleep(100);
        }
        check(network!=null,"Embedded bridge did not provide a validated network");
        java.net.HttpURLConnection http=(java.net.HttpURLConnection)network.openConnection(new java.net.URL("https://example.com/"));
        http.setConnectTimeout(10000);http.setReadTimeout(10000);
        int status;try{status=http.getResponseCode();check(status==200,"Network-bound HTTPS failed: "+status);}
        finally{http.disconnect();}
        try(LocalSocket socket=new LocalSocket()) {
            socket.connect(new LocalSocketAddress("/dev/socket/pocket_control32",LocalSocketAddress.Namespace.FILESYSTEM));
            socket.setSoTimeout(3000);check(socket.getInputStream().read()==-1,"An app impersonated the host input owner");
        }
        return new JSONObject().put("validated",true).put("networkBound",true).put("httpStatus",status).put("inputPeerDenied",true);
    }
    private static void check(boolean value,String reason)throws IOException {if(!value)throw new IOException(reason);}
    private static final class Completion {
        final CompletableFuture<Bundle> result=new CompletableFuture<>();final Object callback;
        Completion()throws Exception {
            Class<?> api=Class.forName("android.os.RemoteCallback"),listener=Class.forName("android.os.RemoteCallback$OnResultListener");
            Object proxy=java.lang.reflect.Proxy.newProxyInstance(listener.getClassLoader(),new Class<?>[]{listener},(p,m,a)->{
                if(m.getName().equals("onResult")){result.complete((Bundle)a[0]);return null;}
                if(m.getName().equals("toString"))return "MediaSessionCompletion";
                if(m.getName().equals("hashCode"))return System.identityHashCode(p);
                if(m.getName().equals("equals"))return p==a[0];return null;});
            callback=api.getConstructor(listener).newInstance(proxy);
        }
        void await()throws Exception {
            Bundle bundle=result.get(15,TimeUnit.SECONDS);
            String key=(String)Class.forName("android.service.storage.ExternalStorageService").getField("EXTRA_ERROR").get(null);
            check(bundle!=null && bundle.getParcelable(key)==null,"ExternalStorageService callback error: "+bundle);
        }
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);setTurnScreenOn(true);
        TextView view=new TextView(this);view.setText("Testing real12L MediaProvider session…");setContentView(view);
        if(new File("/tmp/pocket-bridge-probe").isFile()) {
            try {
                inputRun=new String(Files.readAllBytes(new File("/tmp/pocket-bridge-probe").toPath()),"US-ASCII");
                check(inputRun.matches("[a-f0-9]{32}"),"Invalid bridge input nonce");
            }catch(Exception error){throw new IllegalStateException(error);}
            view.setOnTouchListener((target,event)->{
                if(event.getActionMasked()==android.view.MotionEvent.ACTION_UP)
                    Log.i(TAG,"POCKET_MEDIA_INPUT32_TOUCH run="+inputRun+" uid="+Os.getuid());
                return true;
            });
        }
        new Thread(()->run(view),"MediaProviderSession32").start();
    }
    private void run(TextView view) {
        JSONObject report=new JSONObject();java.lang.Process child=null;LocalSocket listener=null,peer=null;LocalServerSocket server=null;
        FileDescriptor clientFd=null;Object service=null;Class<?> serviceApi=null,callbackApi=null;boolean bound=false;
        String run="",session="";CompletableFuture<IBinder> connected=new CompletableFuture<>();
        ServiceConnection connection=new ServiceConnection(){
            public void onServiceConnected(ComponentName name,IBinder binder){connected.complete(binder);}
            public void onServiceDisconnected(ComponentName name){connected.completeExceptionally(new IOException("MediaProvider service died"));}
        };
        try {
            check(Build.VERSION.SDK_INT==32 && Os.getuid()>=10000,"Expected ordinary API32 guest app");
            run=new String(Files.readAllBytes(new File("/tmp/pocket-media-run").toPath()),"US-ASCII").trim();check(run.matches("[a-f0-9]{32}"),"Missing invocation nonce");session="pocket-probe32-"+run;
            report.put("runId",run).put("uid",Os.getuid()).put("sdk",Build.VERSION.SDK_INT);
            check(getPackageManager().checkPermission("android.permission.BIND_EXTERNAL_STORAGE_SERVICE",getPackageName())==PackageManager.PERMISSION_GRANTED,"Test platform signature permission not granted");
            Intent intent=new Intent().setComponent(new ComponentName("com.android.providers.media.module","com.android.providers.media.fuse.ExternalStorageServiceImpl"));
            bound=bindService(intent,connection,Context.BIND_AUTO_CREATE);check(bound,"Cannot bind real MediaProvider service");
            IBinder binder=connected.get(20,TimeUnit.SECONDS);serviceApi=Class.forName("android.service.storage.IExternalStorageService");callbackApi=Class.forName("android.os.RemoteCallback");
            service=Class.forName("android.service.storage.IExternalStorageService$Stub").getMethod("asInterface",IBinder.class).invoke(null,binder);
            FileDescriptor daemonFd=new FileDescriptor();clientFd=new FileDescriptor();
            Os.class.getMethod("socketpair",int.class,int.class,int.class,FileDescriptor.class,FileDescriptor.class).invoke(null,OsConstants.AF_UNIX,OsConstants.SOCK_SEQPACKET,0,daemonFd,clientFd);
            ParcelFileDescriptor daemon=ParcelFileDescriptor.dup(daemonFd);Os.close(daemonFd);
            listener=new LocalSocket();listener.bind(new LocalSocketAddress("/dev/socket/pms32",LocalSocketAddress.Namespace.FILESYSTEM));server=new LocalServerSocket(listener.getFileDescriptor());
            File output=new File(getFilesDir(),"media-native.log");child=new ProcessBuilder("/tmp/pocket-media-client",run,getPackageName()).redirectErrorStream(true).redirectOutput(output).start();
            final LocalServerSocket accepting=server;FutureTask<LocalSocket> accepted=new FutureTask<>(()->accepting.accept());Thread waiter=new Thread(accepted,"MediaSessionAccept");waiter.setDaemon(true);waiter.start();
            peer=accepted.get(15,TimeUnit.SECONDS);peer.setSoTimeout(20000);
            byte[] nonce=new byte[32];new DataInputStream(peer.getInputStream()).readFully(nonce);check(run.equals(new String(nonce,"US-ASCII")),"Wrong native child invocation");
            peer.setFileDescriptorsForSend(new FileDescriptor[]{clientFd});peer.getOutputStream().write('F');peer.setFileDescriptorsForSend(null);Os.close(clientFd);clientFd=null;
            Completion started=new Completion();
            try{serviceApi.getMethod("startSession",String.class,int.class,ParcelFileDescriptor.class,String.class,String.class,callbackApi).invoke(service,session,3,daemon,"/storage/emulated","/data/media",started.callback);}
            finally{daemon.close();}
            // Native client sends INIT before waiting for the framework callback.
            check(peer.getInputStream().read()=='I',"Real MediaProvider INIT did not complete");started.await();report.put("startCallback",true);
            peer.getOutputStream().write('G');int phase=peer.getInputStream().read();
            if(phase=='N') {
                // The namespace workload includes many synchronous provider
                // and database round trips, before the host proxy starts.
                peer.setSoTimeout(90000);check(peer.getInputStream().read()=='S',"Real provider namespace operations failed");
                report.put("namespaceProof",true);phase=peer.getInputStream().read();
            }
            if(new File("/tmp/pocket-media-proxy").isFile()) {
                check(phase=='P',"Real file broker did not start");report.put("dataProof",DataProof.run(run));peer.getOutputStream().write('R');phase=peer.getInputStream().read();
            }
            check(phase=='D',"Real MediaProvider operations failed");
            check(child.waitFor(15,TimeUnit.SECONDS) && child.exitValue()==0,"Native MediaProvider client did not exit0");
            check(output.length()<65536,"Native report bound");String text=new String(Files.readAllBytes(output.toPath()),"UTF-8");report.put("nativeLog",text).put("nativeExit",child.exitValue());
            check(text.contains("MEDIA_FUSE32_PASS"),"Real MediaProvider proof marker missing");
            Completion ended=new Completion();serviceApi.getMethod("endSession",String.class,callbackApi).invoke(service,session,ended.callback);ended.await();service=null;
            String scope=new File("/.pocket-external-enabled").isFile()?"Real MediaProvider session, ordinary file opens through loader/app broker/host AppFuse, data and private cross-app open denial; directory metadata, per-app private lower views, full mount/MediaStore and cache coherence remain unverified":new File("/tmp/pocket-media-proxy").isFile()?"Real MediaProvider session and data through supplied host AppFuse FDs; production mounted paths remain unverified":"Real MediaProvider session, user-directory metadata and stock app-private FUSE denial; file data and mounted paths remain unverified";
            report.put("endCallback",true).put("validation","PASS").put("scope",scope);
            if(inputRun!=null)report.put("bridgeNetwork",bridgeNetwork());
        }catch(Throwable error){try{
            while(error instanceof InvocationTargetException && error.getCause()!=null)error=error.getCause();
            report.put("validation","FAIL").put("error",error.toString());
            File output=new File(getFilesDir(),"media-native.log");if(output.isFile() && output.length()<65536)report.put("nativeLog",new String(Files.readAllBytes(output.toPath()),"UTF-8"));
        }catch(Exception ignored){}}
        finally {
            if(child!=null && child.isAlive()){child.destroyForcibly();try{child.waitFor(5,TimeUnit.SECONDS);}catch(Exception ignored){}}
            if(peer!=null)try{peer.close();}catch(Exception ignored){}
            if(clientFd!=null)try{Os.close(clientFd);}catch(Exception ignored){}
            if(server!=null)try{server.close();}catch(Exception ignored){}
            if(listener!=null)try{listener.close();}catch(Exception ignored){}
            if(service!=null)try{Completion ended=new Completion();serviceApi.getMethod("endSession",String.class,callbackApi).invoke(service,session,ended.callback);ended.await();report.put("endCallback",true);}
                catch(Exception error){try{report.put("cleanupError",error.toString()).put("validation","FAIL");}catch(Exception ignored){}}
            if(bound)unbindService(connection);
        }
        final String text=report.toString();Log.i(TAG,"POCKET_MEDIA_SESSION32_RESULT "+text);runOnUiThread(()->view.setText(text));
    }
}
