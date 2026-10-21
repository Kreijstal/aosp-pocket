// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.app.Activity;
import android.content.*;
import android.database.Cursor;
import android.net.*;
import android.os.*;
import android.view.KeyEvent;
import android.widget.TextView;
import java.io.*;
import java.lang.reflect.Field;
import java.util.*;
import java.util.concurrent.*;
import dev.kreijstal.aosppocket.bridge.DisplayGeometry;
import org.json.*;

/** Runs the production host command path against a deliberately stalled socket peer. */
public final class InstallInputTest extends Activity {
    private static final DisplayGeometry GEOMETRY=new DisplayGeometry(720,1280,320);
    private static volatile CountDownLatch copying, releaseCopy;
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView view=new TextView(this); view.setText("Testing input during APK import…"); setContentView(view);
        new Thread(() -> {
            JSONObject report=new JSONObject();
            try {
                JSONObject copy=check(true), commit=check(false);
                report.put("classification","synthetic_only").put("kernel_uid",android.os.Process.myUid());
                report.put("copy",copy).put("commit",commit);
                report.put("passed",copy.getBoolean("input_while_blocked") && commit.getBoolean("input_while_blocked"));
            } catch(Throwable error) {
                try { report.put("passed",false).put("error",error.toString()); } catch(JSONException ignored) { }
            }
            try(FileOutputStream out=openFileOutput("result.json",0)) { out.write(report.toString().getBytes("UTF-8")); }
            catch(IOException error) { throw new RuntimeException(error); }
            String text=report.toString(); android.util.Log.i("PocketInstallInputTest",text);
            runOnUiThread(() -> view.setText(text));
        },"InstallInputTest").start();
    }
    private JSONObject check(boolean slowCopy) throws Exception {
        File root=new File(getFilesDir(),slowCopy ? "copy" : "commit");
        new File(root,"dev/socket").mkdirs(); new File(root,"system-data").mkdirs();
        File socketPath=new File(root,"dev/socket/pocket_control"); socketPath.delete();
        LocalSocket bound=new LocalSocket();
        bound.bind(new LocalSocketAddress(socketPath.getPath(),LocalSocketAddress.Namespace.FILESYSTEM));
        LocalServerSocket server=new LocalServerSocket(bound.getFileDescriptor());
        CountDownLatch install=new CountDownLatch(1), release=new CountDownLatch(slowCopy ? 0 : 1);
        CountDownLatch key=new CountDownLatch(1), installed=new CountDownLatch(1);
        List<LocalSocket> peers=new CopyOnWriteArrayList<>();
        Thread accept=new Thread(() -> {
            try {
                while(true) {
                    LocalSocket peer=server.accept(); peers.add(peer);
                    new Thread(() -> {
                        try(LocalSocket connection=peer) {
                            DataInputStream in=new DataInputStream(connection.getInputStream());
                            DataOutputStream out=new DataOutputStream(connection.getOutputStream());
                            GEOMETRY.write(out); out.flush();
                            while(true) {
                                int op=in.readInt();
                                if(op==4) { install.countDown(); release.await(); out.writeUTF("Installed dev.kreijstal.fixture"); }
                                else if(op==3) { in.readInt(); key.countDown(); out.writeBoolean(true); }
                                else throw new IOException("Unexpected command "+op);
                                out.flush();
                            }
                        } catch(Exception closed) { /* Test connection teardown. */ }
                    },"InstallInputPeer").start();
                }
            } catch(IOException closed) { /* Test listener teardown. */ }
        },"InstallInputServer"); accept.start();
        DirectSession session=new DirectSession(this,GEOMETRY,60,new DirectSession.Listener() {
            public void status(String value) { if(value.startsWith("Installed ")) installed.countDown(); }
            public void ready() { }
            public void stopped(String value) { }
        });
        Field location=DirectSession.class.getDeclaredField("root"); location.setAccessible(true); location.set(session,root);
        Field ready=DirectSession.class.getDeclaredField("ready"); ready.setAccessible(true); ready.setBoolean(session,true);
        try {
            Uri source;
            copying=new CountDownLatch(1); releaseCopy=new CountDownLatch(1);
            if(slowCopy) source=Uri.parse("content://dev.kreijstal.pocketinstalltest.stall/base.apk");
            else {
                File apk=new File(root,"fixture.apk");
                try(FileOutputStream out=new FileOutputStream(apk)) { out.write(new byte[4096]); }
                source=Uri.fromFile(apk);
            }
            session.install(Collections.singletonMap("base.apk",source));
            if(!(slowCopy ? copying : install).await(10,TimeUnit.SECONDS)) throw new IOException("Import did not enter blocked phase");
            long began=SystemClock.elapsedRealtime(); session.key(KeyEvent.KEYCODE_HOME);
            boolean responsive=key.await(3,TimeUnit.SECONDS); long delay=SystemClock.elapsedRealtime()-began;
            releaseCopy.countDown(); release.countDown();
            if(!installed.await(10,TimeUnit.SECONDS)) throw new IOException("Import did not finish after release");
            session.key(KeyEvent.KEYCODE_BACK);
            return new JSONObject().put("input_while_blocked",responsive).put("input_delay_ms",delay).put("install_completed_after_release",true);
        } finally {
            releaseCopy.countDown(); release.countDown(); session.close();
            server.close(); bound.close(); for(LocalSocket peer:peers) peer.close();
            accept.join(2000); socketPath.delete();
        }
    }
    public static final class StallProvider extends ContentProvider {
        public boolean onCreate() { return true; }
        public ParcelFileDescriptor openFile(Uri uri,String mode) throws FileNotFoundException {
            try {
                ParcelFileDescriptor[] pipe=ParcelFileDescriptor.createPipe();
                new Thread(() -> {
                    try(OutputStream out=new ParcelFileDescriptor.AutoCloseOutputStream(pipe[1])) {
                        out.write(new byte[4096]); copying.countDown(); releaseCopy.await(); out.write(1);
                    } catch(Exception closed) { /* Test cancellation. */ }
                },"BlockedApkSource").start();
                return pipe[0];
            } catch(IOException error) { throw new FileNotFoundException(error.toString()); }
        }
        public String getType(Uri uri) { return "application/vnd.android.package-archive"; }
        public Cursor query(Uri u,String[] p,String s,String[] a,String o) { return null; }
        public Uri insert(Uri u,ContentValues v) { throw new UnsupportedOperationException(); }
        public int update(Uri u,ContentValues v,String s,String[] a) { throw new UnsupportedOperationException(); }
        public int delete(Uri u,String s,String[] a) { throw new UnsupportedOperationException(); }
    }
}
