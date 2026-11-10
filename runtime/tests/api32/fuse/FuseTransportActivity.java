// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import android.app.Activity;
import android.os.Bundle;
import android.system.Os;
import android.widget.TextView;
import java.io.*;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.concurrent.*;
import org.json.*;

/** Isolated native12L libfuse transport proof; no guest volume is announced. */
public final class FuseTransportActivity extends Activity {
    private volatile String report="{}";
    private static String boundedText(File file,int bound)throws IOException {
        if(file.length()>bound)throw new IOException("Fixture text exceeds bound: "+file.getName());
        return new String(Files.readAllBytes(file.toPath()),"UTF-8");
    }
    private void directoryClient(File root,String run,Process parent,JSONObject result)throws Exception {
        Process child=null;JSONObject state=new JSONObject();File log=new File(getFilesDir(),"directory-libc-child-"+run+".log");
        String prefix="tmp/directory-child-"+run;File done=new File(root,prefix+"-done");boolean success=false;
        try {
            File ready=new File(root,prefix+"-ready.json");long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(30);
            while(!ready.isFile()) {
                if(!parent.isAlive())throw new IOException("Native parent exited before directory client readiness");
                if(System.nanoTime()>=deadline)throw new IOException("Directory client readiness timed out");
                Thread.sleep(20);
            }
            JSONObject request=new JSONObject(boundedText(ready,4096));
            if(!run.equals(request.getString("runId")))throw new IOException("Directory client run differs");
            File childRoot=new File(request.getString("root"));File canonical=childRoot.getCanonicalFile();
            File temp=new File(root,"tmp").getCanonicalFile();
            if(!childRoot.equals(canonical) || !temp.equals(canonical.getParentFile()) || !canonical.getName().matches("n[A-Za-z0-9]{6}"))throw new IOException("Directory child root escapes fixture");
            if(!new File(childRoot,".pocket-owner").mkdir())throw new IOException("Directory child owner already exists");
            NativeLoader.prepare(childRoot);
            ProcessBuilder builder=new ProcessBuilder(new File(childRoot,"system/bin/pocket-loader").getPath(),"--libc-directory-client").directory(childRoot);
            builder.environment().clear();builder.environment().put("POCKET_LOADER_BOOT","1");
            builder.environment().put("POCKET_LOADER_EXEC_V1",new File(childRoot,"system/bin/fuse-stock").getPath());
            builder.environment().put("LD_LIBRARY_PATH",new File(childRoot,"system/lib64").getPath());
            builder.redirectErrorStream(true).redirectOutput(log);child=builder.start();
            if(!child.waitFor(30,TimeUnit.SECONDS))throw new IOException("Exact directory libc child timed out");
            String text=boundedText(log,65536);state.put("exit",child.exitValue()).put("log",text);
            if(child.exitValue()!=0 || !text.contains("DIRECTORY_LIBC_CLIENT_PASS"))throw new IOException("Directory libc child proof failed");
            success=true;state.put("validation","PASS");
        }catch(Throwable error) {
            state.put("validation","FAIL").put("error",error.toString());
        }finally {
            if(child!=null && child.isAlive()){child.destroyForcibly();child.waitFor(5,TimeUnit.SECONDS);}
            if(child!=null && !child.isAlive())state.put("exit",child.exitValue());
            if(log.isFile())state.put("log",boundedText(log,65536));
            result.put("directoryLibcChild",state);
            File next=new File(root,prefix+"-done.new");
            Files.write(next.toPath(),(success?"0\n":"1\n").getBytes("UTF-8"));
            Files.move(next.toPath(),done.toPath(),StandardCopyOption.ATOMIC_MOVE);
        }
    }
    @Override public void dump(String prefix,FileDescriptor fd,PrintWriter out,String[] args) {
        out.println("POCKET_FUSE32="+android.util.Base64.encodeToString(report.getBytes(java.nio.charset.StandardCharsets.UTF_8),android.util.Base64.NO_WRAP));
    }
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);TextView view=new TextView(this);view.setText("Testing Android12L FUSE transport…");setContentView(view);
        String run=getIntent().getStringExtra("probe_run_id");if(run==null || !run.matches("[a-f0-9]{32}")){finish();return;}
        new Thread(()-> {
            JSONObject result=new JSONObject();java.lang.Process process=null;
            try {
                result.put("runId",run).put("hostUid",android.os.Process.myUid());
                boolean namespace=getPackageName().equals("dev.kreijstal.aosppocket.api32namespacerpctest");
                File root=new File(getFilesDir(),namespace?"n":"fuse-root");root.mkdirs();new File(root,"tmp").mkdirs();
                JSONObject inventory,kind;
                try(InputStream input=getAssets().open("fuse-probe-kind.json")){ByteArrayOutputStream bytes=new ByteArrayOutputStream();byte[] b=new byte[1024];int n;while((n=input.read(b))!=-1){bytes.write(b,0,n);if(bytes.size()>4096)throw new IOException("Fixture kind bound");}kind=new JSONObject(bytes.toString("UTF-8"));}
                try(InputStream input=getAssets().open("fuse-files.json")){ByteArrayOutputStream bytes=new ByteArrayOutputStream();byte[] b=new byte[8192];int n;while((n=input.read(b))!=-1)bytes.write(b,0,n);inventory=new JSONObject(bytes.toString("UTF-8"));}
                for(Iterator<String> keys=inventory.keys();keys.hasNext();) {
                    String name=keys.next();if(name.startsWith("/") || Arrays.asList(name.split("/")).contains(".."))throw new IOException("Invalid fixture path");
                    File file=new File(root,name);file.getParentFile().mkdirs();MessageDigest digest=MessageDigest.getInstance("SHA-256");
                    try(InputStream input=getAssets().open("root/"+name);OutputStream output=new FileOutputStream(file)) {
                        byte[] data=new byte[65536];int n;while((n=input.read(data))!=-1){digest.update(data,0,n);output.write(data,0,n);}
                    }
                    StringBuilder hash=new StringBuilder();for(byte b:digest.digest())hash.append(String.format(Locale.ROOT,"%02x",b&255));
                    if(!hash.toString().equals(inventory.getString(name)))throw new IOException("Fixture checksum differs");Os.chmod(file.getPath(),0755);
                }
                new File(root,".pocket-owner").mkdirs();new File(root,"dev/socket").mkdirs();NativeLoader.prepare(root);
                File log=new File(getFilesDir(),"fuse-stock.log");ProcessBuilder builder=new ProcessBuilder(new File(root,"system/bin/fuse-stock").getPath()).directory(root);
                builder.environment().clear();builder.environment().put("POCKET_LOADER_BOOT","1");builder.environment().put("LD_LIBRARY_PATH",new File(root,"system/lib64").getPath());
                builder.environment().put("POCKET_TEST_RUN",run);
                builder.redirectErrorStream(true).redirectOutput(log);process=builder.start();
                if(kind.optBoolean("directoryLibcChild"))directoryClient(root,run,process,result);
                if(!process.waitFor(90,TimeUnit.SECONDS))throw new IOException("Exact native FUSE process timed out");
                if(log.length()>65536)throw new IOException("Native FUSE log bound");String text=new String(Files.readAllBytes(log.toPath()),"UTF-8");
                result.put("nativeExit",process.exitValue()).put("log",text);
                if(process.exitValue()!=0 || !text.contains(namespace?"EXTERNAL_NAMESPACE_RPC_PASS":"FUSE_STOCK32_PASS"))throw new IOException("Native FUSE proof failed");
                result.put("validation","PASS").put("scope",namespace?"12L Bionic, loader and authenticated namespace RPC with a real-file-backed FUSE test peer; no real MediaProvider policy, host AppFuse proxy mount or production mounted-volume claim":"Unmodified packaged12L libfuse and Bionic over rootless packet transport; test callbacks only, not MediaProvider access policy or a mounted guest volume");
            }catch(Throwable error){try{result.put("validation","FAIL").put("error",error.toString());}catch(Exception ignored){}}
            finally{try{if(process!=null && process.isAlive()){process.destroyForcibly();process.waitFor(5,TimeUnit.SECONDS);}File log=new File(getFilesDir(),"fuse-stock.log");if(log.isFile())result.put("log",boundedText(log,65536));if(process!=null && !process.isAlive())result.put("nativeExit",process.exitValue());}catch(Exception ignored){}}
            report=result.toString();try(FileOutputStream output=openFileOutput("report.json",MODE_PRIVATE)){output.write(report.getBytes("UTF-8"));}catch(Exception ignored){}
            runOnUiThread(()->view.setText(report));
        },"PocketFuse32Validation").start();
    }
}
