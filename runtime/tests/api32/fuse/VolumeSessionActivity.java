// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import android.app.Activity;
import android.os.Bundle;
import android.system.Os;
import android.widget.TextView;
import java.io.*;
import java.nio.file.*;
import java.nio.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.concurrent.TimeUnit;
import org.json.*;

/** Real host AppFuse/session adapter; the native FUSE peer is a test model. */
public final class VolumeSessionActivity extends Activity {
    private volatile String report="{}";
    @Override public void dump(String prefix,FileDescriptor fd,PrintWriter out,String[] args){out.println("POCKET_FUSE32="+android.util.Base64.encodeToString(report.getBytes(java.nio.charset.StandardCharsets.UTF_8),android.util.Base64.NO_WRAP));}
    private static String text(File file)throws Exception {if(file.length()>65536)throw new IOException("Fixture log bound");return new String(Files.readAllBytes(file.toPath()),"UTF-8");}
    private static void interpreter(File executable,File loader)throws Exception {
        byte[] bytes=Files.readAllBytes(executable.toPath());ByteBuffer elf=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);long phoff=elf.getLong(32);int stride=elf.getShort(54)&65535,count=elf.getShort(56)&65535;
        for(int i=0;i<count;i++){int at=Math.toIntExact(phoff+(long)i*stride);if(elf.getInt(at)==3){int start=Math.toIntExact(elf.getLong(at+8)),size=Math.toIntExact(elf.getLong(at+32));byte[] path=loader.getPath().getBytes("UTF-8");if(path.length>=size || start>bytes.length-size)throw new IOException("Interpreter reservation bound");Arrays.fill(bytes,start,start+size,(byte)0);System.arraycopy(path,0,bytes,start,path.length);Files.write(executable.toPath(),bytes);return;}}
        throw new IOException("Missing interpreter reservation");
    }
    private static void waitFile(File file,Process process)throws Exception {long end=System.nanoTime()+TimeUnit.SECONDS.toNanos(35);while(!file.isFile()){if(!process.isAlive())throw new IOException("Exact process exited before "+file.getName());if(System.nanoTime()>end)throw new IOException("Observation timed out: "+file.getName());Thread.sleep(20);}if(!text(file).equals("0\n"))throw new IOException("Fixture marker differs");}
    private Process start(File root,File log,String run,boolean client)throws Exception {
        ProcessBuilder builder=client?new ProcessBuilder(new File(root,"system/bin/pocket-loader").getPath(),"--app-client"):new ProcessBuilder(new File(root,"system/bin/pocket-core-api32").getPath());
        builder.directory(root);builder.environment().clear();builder.environment().put("POCKET_LOADER_BOOT","1");builder.environment().put("POCKET_TEST_RUN",run);builder.environment().put("LD_LIBRARY_PATH",new File(root,"system/lib64").getPath());
        builder.environment().put("POCKET_API32_HOST_PID",Integer.toString(android.os.Process.myPid()));
        if(getIntent().getBooleanExtra("detach_retry",false))builder.environment().put("POCKET_TEST_DETACH_RETRY","1");
        String stat=text(new File("/proc/"+android.os.Process.myPid()+"/stat"));builder.environment().put("POCKET_API32_HOST_START",stat.substring(stat.lastIndexOf(')')+2).trim().split(" +")[19]);
        if(client)builder.environment().put("POCKET_LOADER_EXEC_V1",new File(root,"system/bin/pocket-core-api32").getPath());
        builder.redirectErrorStream(true).redirectOutput(log);return builder.start();
    }
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);TextView view=new TextView(this);view.setText("Testing12L volume adapter…");setContentView(view);
        String run=getIntent().getStringExtra("probe_run_id");if(run==null || !run.matches("[a-f0-9]{32}")){finish();return;}
        if(getIntent().getBooleanExtra("holder_probe_only",false)) {
            try{ExternalFuse32.checkRetirementSupport();report=new JSONObject().put("runId",run).put("hostUid",android.os.Process.myUid())
                .put("pidfdOpen",true).put("pidfdSignalZero",true).put("guestStarted",false).put("validation","PASS").toString();}
            catch(Exception error){try{report=new JSONObject().put("runId",run).put("validation","FAIL").put("error",error.toString()).toString();}catch(Exception ignored){}}
            view.setText(report);return;
        }
        new Thread(()->{
            JSONObject result=new JSONObject();Process core=null,client=null;ExternalVolume32 volume=null;File coreLog=new File(getFilesDir(),"volume-core-"+run+".log"),clientLog=new File(getFilesDir(),"volume-client-"+run+".log");
            try {
                result.put("runId",run).put("hostUid",android.os.Process.myUid());File root=new File(getFilesDir(),"v"+run.substring(0,8));if(!root.mkdir())throw new IOException("Fresh fixture root already exists");new File(root,"tmp").mkdirs();new File(root,"dev/socket").mkdirs();new File(root,".pocket-owner").mkdirs();
                JSONObject inventory;try(InputStream input=getAssets().open("fuse-files.json")){ByteArrayOutputStream bytes=new ByteArrayOutputStream();byte[] data=new byte[8192];int n;while((n=input.read(data))!=-1)bytes.write(data,0,n);inventory=new JSONObject(bytes.toString("UTF-8"));}
                for(Iterator<String> keys=inventory.keys();keys.hasNext();) {
                    String name=keys.next();if(name.startsWith("/") || Arrays.asList(name.split("/")).contains(".."))throw new IOException("Invalid fixture path");File file=new File(root,name);file.getParentFile().mkdirs();MessageDigest digest=MessageDigest.getInstance("SHA-256");
                    try(InputStream input=getAssets().open("root/"+name);OutputStream output=new FileOutputStream(file)){byte[] data=new byte[65536];int n;while((n=input.read(data))!=-1){digest.update(data,0,n);output.write(data,0,n);}}
                    StringBuilder actual=new StringBuilder();for(byte b:digest.digest())actual.append(String.format(Locale.ROOT,"%02x",b&255));if(!actual.toString().equals(inventory.getString(name)))throw new IOException("Fixture checksum differs");Os.chmod(file.getPath(),0755);
                }
                NativeLoader.prepare(root);interpreter(new File(root,"system/bin/pocket-core-api32"),new File(root,"system/bin/pocket-loader"));volume=new ExternalVolume32(root);core=start(root,coreLog,run,false);
                String prefix="tmp/volume-"+run;waitFile(new File(root,prefix+"-ready"),core);client=start(root,clientLog,run,true);waitFile(new File(root,prefix+"-client-ready"),client);
                Files.write(new File(root,prefix+"-revoke").toPath(),"0\n".getBytes("US-ASCII"),StandardOpenOption.CREATE_NEW);
                if(!client.waitFor(40,TimeUnit.SECONDS) || !core.waitFor(40,TimeUnit.SECONDS))throw new IOException("Exact volume processes timed out");
                result.put("nativeExit",core.exitValue()).put("clientExit",client.exitValue()).put("log",text(coreLog)).put("clientLog",text(clientLog));
                volume.close();result.put("adapter",volume.report());
                java.util.regex.Matcher holder=java.util.regex.Pattern.compile("VOLUME_CLIENT32_READY pid=([1-9][0-9]*) ").matcher(text(clientLog));
                if(!holder.find())throw new IOException("Missing cached-holder identity");int holderPid=Integer.parseInt(holder.group(1));result.put("holderPid",holderPid);
                JSONArray retired=volume.report().getJSONArray("retiredHolders");boolean matched=false;for(int i=0;i<retired.length();i++)if(retired.getInt(i)==holderPid)matched=true;
                if(core.exitValue()!=0 || client.exitValue()!=137 || !matched || !text(coreLog).contains("VOLUME_SESSION32_PASS") || text(clientLog).contains("VOLUME_CLIENT32_NOT_RETIRED"))throw new IOException("Real volume holder retirement proof failed");
                if(getIntent().getBooleanExtra("detach_retry",false) && (volume.report().getInt("replyFailures")!=1 || !text(coreLog).contains("VOLUME_DETACH_RETRY32_PASS")))throw new IOException("Lost-reply detach retry proof failed");
                result.put("validation","PASS");
            }catch(Throwable error){try{result.put("validation","FAIL").put("error",error.toString());}catch(Exception ignored){}}
            finally {
                try{if(client!=null && client.isAlive()){client.destroyForcibly();client.waitFor(5,TimeUnit.SECONDS);}if(core!=null && core.isAlive()){core.destroyForcibly();core.waitFor(5,TimeUnit.SECONDS);}if(volume!=null){volume.close();result.put("adapter",volume.report());}}catch(Exception error){try{result.put("cleanupError",error.toString()).put("validation","FAIL");}catch(Exception ignored){}}
                try{if(coreLog.isFile())result.put("log",text(coreLog));if(clientLog.isFile())result.put("clientLog",text(clientLog));if(core!=null && !core.isAlive())result.put("nativeExit",core.exitValue());if(client!=null && !client.isAlive())result.put("clientExit",client.exitValue());}catch(Exception ignored){}
            }
            report=result.toString();try(FileOutputStream file=openFileOutput("report.json",MODE_PRIVATE)){file.write(report.getBytes("UTF-8"));}catch(Exception ignored){}runOnUiThread(()->view.setText(report));
        },"PocketVolumeValidation").start();
    }
}
