// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketmediasession32;
import android.net.*;
import android.os.*;
import android.system.*;
import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.util.concurrent.*;
import org.json.*;

/** Ordinary app open(2), not test-injected file descriptors. */
final class PathProof {
    // ARM64 asm/fcntl.h; not exposed by the public OsConstants SDK stub.
    private static final int O_DIRECT=0200000;
    private static void check(boolean value,String reason)throws IOException{if(!value)throw new IOException(reason);}
    private static void denied(String path)throws Exception {
        try{FileDescriptor fd=Os.open(path,OsConstants.O_RDONLY,0);Os.close(fd);throw new IOException("Scoped open succeeded: "+path);}
        catch(ErrnoException expected){check(expected.errno==OsConstants.EACCES || expected.errno==OsConstants.ENOENT || expected.errno==OsConstants.EPERM,"Wrong scoped open errno: "+expected.errno);}
    }
    private static void extractor(JSONObject result)throws Exception {
        if(!new File("/tmp/pocket-lazy-media-probe").isFile())return;
        android.content.Context context=(android.content.Context)Class.forName("android.app.ActivityThread").getMethod("currentApplication").invoke(null);
        File file=File.createTempFile("pocket-extractor-",".wav",context.getCacheDir());
        android.media.MediaExtractor media=new android.media.MediaExtractor();
        try {
            ByteBuffer wav=ByteBuffer.allocate(364).order(ByteOrder.LITTLE_ENDIAN);
            wav.put("RIFF".getBytes("US-ASCII")).putInt(356).put("WAVEfmt ".getBytes("US-ASCII")).putInt(16).putShort((short)1).putShort((short)1).putInt(8000).putInt(16000).putShort((short)2).putShort((short)16).put("data".getBytes("US-ASCII")).putInt(320);
            Files.write(file.toPath(),wav.array());media.setDataSource(file.getPath());
            check(media.getTrackCount()==1,"Lazy extractor lost WAV track");android.media.MediaFormat format=media.getTrackFormat(0);
            check("audio/raw".equals(format.getString(android.media.MediaFormat.KEY_MIME)) && format.getInteger(android.media.MediaFormat.KEY_SAMPLE_RATE)==8000 && format.getInteger(android.media.MediaFormat.KEY_CHANNEL_COUNT)==1,"Lazy extractor changed WAV format");
            media.selectTrack(0);ByteBuffer samples=ByteBuffer.allocate(320);int count=media.readSampleData(samples,0);
            check(count==320,"Lazy extractor sample size differs: "+count);
            for(int n=0;n<count;n++)check(samples.get(n)==0,"Lazy extractor changed PCM bytes");
            result.put("lazyExtractorRead",true).put("extractorSampleBytes",count);
        }finally{media.release();Files.deleteIfExists(file.toPath());}
    }
    static JSONObject run(String nonce)throws Exception {
        JSONObject result=new JSONObject();
        extractor(result);
        for(String endpoint:new String[]{"pocket_ext","pocket_xfd"})try(LocalSocket forged=new LocalSocket()) {
            forged.connect(new LocalSocketAddress("/dev/socket/"+endpoint,LocalSocketAddress.Namespace.FILESYSTEM));forged.setSoTimeout(5000);
            byte[] reply=new byte[endpoint.equals("pocket_ext")?32:8];new DataInputStream(forged.getInputStream()).readFully(reply);
            check(ByteBuffer.wrap(reply).order(ByteOrder.LITTLE_ENDIAN).getInt()==-OsConstants.EACCES,"App impersonated trusted host/broker");
        }
        result.put("hostRpcImpersonationDenied",true).put("attachPeerImpersonationDenied",true);
        String relative="/Android/media/dev.kreijstal.pocketmediasession32/canary";
        FileDescriptor rw=Os.open("/storage/emulated/0"+relative,OsConstants.O_RDWR|OsConstants.O_CLOEXEC,0);
        try {
            StructStatVfs filesystem=Os.fstatvfs(rw);check(filesystem.f_blocks>0 && filesystem.f_bsize>0 && filesystem.f_bavail<=filesystem.f_blocks,"Proxy statfs is invalid: blocks="+filesystem.f_blocks+" bsize="+filesystem.f_bsize+" bfree="+filesystem.f_bfree+" bavail="+filesystem.f_bavail+" frsize="+filesystem.f_frsize);
            result.put("statfs",true).put("filesystemBlocks",filesystem.f_blocks).put("filesystemBlockSize",filesystem.f_bsize);
            FileDescriptor ro=Os.open("/mnt/user/0/emulated/0"+relative,OsConstants.O_RDONLY|O_DIRECT,0);
            try {
                check((Os.fcntlInt(rw,OsConstants.F_GETFD,0)&OsConstants.FD_CLOEXEC)!=0 && (Os.fcntlInt(ro,OsConstants.F_GETFD,0)&OsConstants.FD_CLOEXEC)==0,"open CLOEXEC flags changed");
                String expected="pocket-real-media-"+nonce;byte[] bytes=new byte[64];
                check(Os.pread(rw,bytes,0,expected.length(),0)==expected.length() && expected.equals(new String(bytes,0,expected.length(),"UTF-8")),"Ordinary path read differs");
                byte[] update="proxy-write".getBytes("UTF-8");check(Os.pwrite(rw,update,0,update.length,128)==update.length,"Ordinary path write failed");
                long mapping=Os.mmap(0,8192,OsConstants.PROT_READ|OsConstants.PROT_WRITE,OsConstants.MAP_SHARED,rw,0);
                try{byte[] mapped="shared-media-map".getBytes("UTF-8");Class.forName("libcore.io.Memory").getMethod("pokeByteArray",long.class,byte[].class,int.class,int.class).invoke(null,mapping+256,mapped,0,mapped.length);Os.msync(mapping,8192,OsConstants.MS_SYNC);}
                finally{Os.munmap(mapping,8192);}
                Os.fdatasync(rw);Os.fsync(rw);result.put("mainTid",android.os.Process.myTid());
                FutureTask<Integer> task=new FutureTask<>(()->{
                    FileDescriptor alias=Os.open("/sdcard"+relative,OsConstants.O_RDONLY|O_DIRECT|OsConstants.O_CLOEXEC,0);
                    try {
                        byte[] value=new byte[11];check(Os.pread(ro,value,0,1,4096)==1 && value[0]=='Q',"Forced path direct read differs");
                        check(Os.pread(alias,value,0,value.length,128)==value.length && new String(value,"UTF-8").equals("proxy-write"),"sdcard alias missed synced write");
                    }finally{Os.close(alias);}
                    return android.os.Process.myTid();
                });Thread worker=new Thread(task,"MediaPathWorker");worker.start();result.put("workerTid",task.get(15,TimeUnit.SECONDS));worker.join();
                try{Os.pwrite(ro,update,0,update.length,0);throw new IOException("Read-only path accepted write");}catch(ErrnoException expectedError){check(expectedError.errno==OsConstants.EBADF,"Read-only errno changed");}
                String other="/0/Android/data/dev.kreijstal.pocketmediasession32.other/files/canary";
                denied("/storage/emulated"+other);denied("/data/media"+other);
                result.put("ordinaryPathOpen",true).put("openFlagsPreserved",true).put("aliasRead",true).put("crossPackageDenied",true).put("backingAliasDenied",true).put("uid",Os.getuid()).put("readWrite",true).put("sharedMmap",true).put("readOnlyDenied",true);
            }finally{Os.close(ro);}
        }finally{Os.close(rw);}
        java.nio.file.Path complete=new File("/tmp/media-proxy-app-done").toPath(),temporary=new File("/tmp/media-proxy-app-done.tmp").toPath();
        Files.write(temporary,nonce.getBytes("US-ASCII"),StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE);
        Files.move(temporary,complete,StandardCopyOption.ATOMIC_MOVE);
        long until=SystemClock.elapsedRealtime()+30000;File done=new File("/tmp/media-proxy-done");
        while(!done.isFile()){check(SystemClock.elapsedRealtime()<until,"Host did not release path files");Thread.sleep(20);}
        check(nonce.equals(new String(Files.readAllBytes(done.toPath()),"US-ASCII")),"Wrong path completion nonce");return result;
    }
}
