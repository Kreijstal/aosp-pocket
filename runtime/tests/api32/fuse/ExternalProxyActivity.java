// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import android.app.Activity;
import android.os.*;
import android.net.*;
import android.system.*;
import android.widget.TextView;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import org.json.*;

/** Isolated AppFuse data-plane proof. No MediaProvider policy is substituted. */
public final class ExternalProxyActivity extends Activity {
    private volatile String report="{}";
    @Override public void dump(String prefix,FileDescriptor fd,PrintWriter out,String[] args) {
        out.println("POCKET_FUSE32="+android.util.Base64.encodeToString(report.getBytes(java.nio.charset.StandardCharsets.UTF_8),android.util.Base64.NO_WRAP));
    }
    public static final class Backend implements ExternalFuse32.Callbacks {
        final byte[][] files=new byte[4][8192];
        final byte[] data=files[0];
        final CountDownLatch syncEntered=new CountDownLatch(1),syncRelease=new CountDownLatch(1);
        final Set<Integer> readTids=ConcurrentHashMap.newKeySet();
        final Set<Integer> syncFlags=ConcurrentHashMap.newKeySet();
        final Set<Integer> released=ConcurrentHashMap.newKeySet();
        final CountDownLatch allReleased=new CountDownLatch(4);
        volatile int pid;
        Backend(){for(byte[] file:files)Arrays.fill(file,(byte)'Q');}
        private void check(int inode,long handle)throws IOException {
            if(inode<3 || inode>6 || handle!=0x12340000L+inode)throw new IOException("Open handle changed");
        }
        private void caller(int tid)throws Exception {
            if(tid<=0)throw new IOException("Missing synchronous kernel caller tid");
            List<String> status=Files.readAllLines(new File("/proc/"+tid+"/status").toPath());
            boolean found=false;for(String line:status)if(line.startsWith("Tgid:"))found=Integer.parseInt(line.substring(5).trim())==pid;
            if(!found)throw new IOException("Kernel thread belongs to another process");
        }
        public long fuseSize(int inode,int tid)throws Exception {
            if(inode<3 || inode>6)throw new ErrnoException("size",OsConstants.ENOENT);return data.length;
        }
        public long[] fuseOpen(int inode,int tid,int flags)throws Exception {fuseSize(inode,tid);return new long[]{0x12340000L+inode,inode==3?2:1};}
        public synchronized byte[] fuseRead(int inode,long handle,long offset,int size,int tid)throws Exception {
            check(inode,handle);caller(tid);readTids.add(tid);
            if(inode==4)throw new ErrnoException("read",OsConstants.EACCES);
            if(offset<0 || offset>data.length)throw new ErrnoException("read",OsConstants.EINVAL);
            return Arrays.copyOfRange(files[inode-3],(int)offset,(int)Math.min(data.length,offset+size));
        }
        public synchronized int fuseWrite(int inode,long handle,long offset,byte[] bytes,int tid)throws Exception {
            check(inode,handle);
            // Kernel writeback may use pid 0; retain it without inventing a caller.
            if(offset<0 || offset+bytes.length>data.length)throw new ErrnoException("write",OsConstants.EFBIG);
            System.arraycopy(bytes,0,files[inode-3],(int)offset,bytes.length);return bytes.length;
        }
        public void fuseSync(int inode,long handle,int tid,int flags)throws Exception {
            check(inode,handle);caller(tid);syncFlags.add(flags);
            if(inode==5){syncEntered.countDown();if(!syncRelease.await(15,TimeUnit.SECONDS))throw new IOException("Blocked sync deadline");}
        }
        public void fuseFlush(int inode,long handle,int tid)throws Exception {check(inode,handle);}
        public void fuseRelease(int inode,long handle,int tid,int flags)throws Exception {check(inode,handle);if(released.add(inode))allReleased.countDown();}
    }
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);TextView view=new TextView(this);view.setText("Testing Android12L external file descriptors…");setContentView(view);
        String run=getIntent().getStringExtra("probe_run_id");if(run==null || !run.matches("[a-f0-9]{32}")){finish();return;}
        new Thread(()-> {
            JSONObject result=new JSONObject();java.lang.Process process=null;Backend backend=new Backend();
            LocalSocket listener=null;LocalServerSocket server=null;LocalSocket client=null;
            ExternalFuse32 transport=null;ArrayList<ParcelFileDescriptor> descriptors=new ArrayList<>();
            try {
                result.put("runId",run).put("hostUid",android.os.Process.myUid());
                File root=new File(getFilesDir(),"fuse-root");root.mkdirs();JSONObject inventory;
                try(InputStream in=getAssets().open("fuse-files.json")){ByteArrayOutputStream out=new ByteArrayOutputStream();byte[] b=new byte[8192];int n;while((n=in.read(b))!=-1)out.write(b,0,n);inventory=new JSONObject(out.toString("UTF-8"));}
                for(Iterator<String> keys=inventory.keys();keys.hasNext();) {
                    String name=keys.next();if(name.startsWith("/") || Arrays.asList(name.split("/")).contains(".."))throw new IOException("Invalid fixture path");
                    File file=new File(root,name);file.getParentFile().mkdirs();byte[] bytes;
                    try(InputStream in=getAssets().open("root/"+name);ByteArrayOutputStream out=new ByteArrayOutputStream()){
                        byte[] b=new byte[65536];int n;while((n=in.read(b))!=-1)out.write(b,0,n);bytes=out.toByteArray();}
                    byte[] digest=java.security.MessageDigest.getInstance("SHA-256").digest(bytes);StringBuilder hash=new StringBuilder();for(byte b:digest)hash.append(String.format(Locale.ROOT,"%02x",b&255));
                    if(!hash.toString().equals(inventory.getString(name)))throw new IOException("Fixture checksum differs");
                    Files.write(file.toPath(),bytes);Os.chmod(file.getPath(),0755);
                }
                new File(root,".pocket-owner").mkdirs();new File(root,"dev/socket").mkdirs();NativeLoader.prepare(root);
                listener=new LocalSocket();listener.bind(new LocalSocketAddress(new File(root,"dev/socket/external-test").getPath(),LocalSocketAddress.Namespace.FILESYSTEM));
                server=new LocalServerSocket(listener.getFileDescriptor());
                transport=new ExternalFuse32(backend);
                for(int node=3;node<=6;node++)descriptors.add(transport.open(node,node==3?ParcelFileDescriptor.MODE_READ_WRITE:ParcelFileDescriptor.MODE_READ_ONLY));
                File log=new File(getFilesDir(),"fuse-proxy.log");ProcessBuilder builder=new ProcessBuilder(new File(root,"system/bin/fuse-stock").getPath(),run).directory(root);
                builder.environment().clear();builder.environment().put("POCKET_LOADER_BOOT","1");builder.environment().put("LD_LIBRARY_PATH",new File(root,"system/lib64").getPath());
                builder.redirectErrorStream(true).redirectOutput(log);process=builder.start();
                // A bounded accept observes this exact child; no guest restart is used on failure.
                final LocalServerSocket accepting=server;FutureTask<LocalSocket> accepted=new FutureTask<>(()->accepting.accept());
                Thread waiter=new Thread(accepted,"ExternalProxyAccept");waiter.setDaemon(true);waiter.start();client=accepted.get(15,TimeUnit.SECONDS);client.setSoTimeout(20000);
                Credentials peer=client.getPeerCredentials();backend.pid=peer.getPid();
                if(backend.pid<=0 || peer.getUid()!=android.os.Process.myUid() || !process.isAlive())throw new IOException("Unexpected native control peer");
                String stat=new String(Files.readAllBytes(new File("/proc/"+backend.pid+"/stat").toPath()),"UTF-8");
                String[] fields=stat.substring(stat.lastIndexOf(')')+2).trim().split(" +");
                if(Integer.parseInt(fields[1])!=android.os.Process.myPid())throw new IOException("Control peer is not our direct child");
                byte[] nonce=new byte[32];new DataInputStream(client.getInputStream()).readFully(nonce);
                if(!run.equals(new String(nonce,"US-ASCII")))throw new IOException("Wrong native invocation");
                FileDescriptor[] fds=new FileDescriptor[4];for(int i=0;i<4;i++)fds[i]=descriptors.get(i).getFileDescriptor();
                client.setFileDescriptorsForSend(fds);client.getOutputStream().write('F');client.setFileDescriptorsForSend(null);
                if(!backend.syncEntered.await(20,TimeUnit.SECONDS))throw new IOException("Native fsync did not enter callback");
                client.getOutputStream().write('S');if(client.getInputStream().read()!='R')throw new IOException("Read while fsync blocked failed");
                backend.syncRelease.countDown();if(client.getInputStream().read()!='D')throw new IOException("Native completion missing");
                if(!process.waitFor(15,TimeUnit.SECONDS))throw new IOException("Exact native proxy process timed out");
                if(log.length()>65536)throw new IOException("Native proxy log bound");String text=new String(Files.readAllBytes(log.toPath()),"UTF-8");
                result.put("nativeExit",process.exitValue()).put("log",text);
                if(process.exitValue()!=0 || !text.contains("FUSE_PROXY32_PASS"))throw new IOException("Native proxy proof failed");
                for(String label:new String[]{"MAIN_TID=","WORKER_TID="}){
                    int start=text.indexOf(label);if(start<0)throw new IOException("Thread result missing");int end=text.indexOf('\n',start);
                    int tid=Integer.parseInt(text.substring(start+label.length(),end).trim());if(!backend.readTids.contains(tid))throw new IOException("Kernel caller tid not preserved");}
                if(!backend.syncFlags.contains(0) || !backend.syncFlags.contains(1))throw new IOException("fsync/fdatasync flags lost");
                synchronized(backend){if(!new String(backend.data,128,10,"UTF-8").equals("shared-map"))throw new IOException("mmap did not reach backend");}
                for(ParcelFileDescriptor descriptor:descriptors)descriptor.close();descriptors.clear();
                if(!backend.allReleased.await(5,TimeUnit.SECONDS))throw new IOException("Open file handles not released");
                result.put("readTids",new JSONArray(backend.readTids)).put("syncFlags",new JSONArray(backend.syncFlags));
                result.put("releasedHandles",new JSONArray(backend.released));
                result.put("validation","PASS").put("scope","Real host AppFuse FDs in guest Bionic/loader; memory-backed callbacks, not MediaProvider policy or mounted external volume");
            }catch(Throwable error){try{
                result.put("validation","FAIL").put("error",error.toString());
                File log=new File(getFilesDir(),"fuse-proxy.log");if(log.isFile() && log.length()<=65536)result.put("log",new String(Files.readAllBytes(log.toPath()),"UTF-8"));
            }catch(Exception ignored){}}
            finally {
                backend.syncRelease.countDown();if(process!=null && process.isAlive())process.destroyForcibly();
                if(client!=null)try{client.close();}catch(Exception ignored){}
                if(server!=null)try{server.close();}catch(Exception ignored){}
                if(listener!=null)try{listener.close();}catch(Exception ignored){}
                for(ParcelFileDescriptor descriptor:descriptors)try{descriptor.close();}catch(Exception ignored){}
                if(transport!=null)transport.close();
            }
            report=result.toString();try(FileOutputStream output=openFileOutput("report.json",MODE_PRIVATE)){output.write(report.getBytes("UTF-8"));}catch(Exception ignored){}
            runOnUiThread(()->view.setText(report));
        },"PocketExternalProxy32Validation").start();
    }
}
