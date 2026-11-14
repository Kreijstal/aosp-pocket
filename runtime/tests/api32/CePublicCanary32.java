// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.os.Build;
import android.os.SystemClock;
import android.system.Os;
import android.system.StructStat;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.*;
import java.util.Arrays;
import org.json.JSONObject;

/** Test fixture created through MediaProvider, alongside the original private canary. */
public final class CePublicCanary32 {
    private static final String PRIVATE="/data/media/0/.pocket-ce32-canary";
    private static final String PRIVATE_ALIAS="/storage/emulated/0/.pocket-ce32-canary";
    private static void require(boolean value,String message)throws IOException {
        if(!value)throw new IOException(message);
    }
    private static JSONObject stat(String path)throws Exception {
        StructStat s=Os.lstat(path);
        require((s.st_mode&0170000)==0100000,"Canary is not a regular file");
        return new JSONObject().put("mode",s.st_mode&07777).put("uid",s.st_uid).put("gid",s.st_gid)
            .put("size",s.st_size).put("inode",s.st_ino);
    }
    private static byte[] read(String path)throws Exception {
        try(FileChannel channel=FileChannel.open(Paths.get(path),StandardOpenOption.READ,LinkOption.NOFOLLOW_LINKS)) {
            ByteBuffer data=ByteBuffer.allocate(129);
            while(data.hasRemaining() && channel.read(data)!=-1){}
            require(data.position()<=128,"Canary exceeds its size limit");
            return Arrays.copyOf(data.array(),data.position());
        }
    }
    private static void requirePrivateAliasDenied()throws Exception {
        try{read(PRIVATE_ALIAS);throw new IOException("Original private canary became publicly readable");}
        catch(AccessDeniedException expected){}
    }
    public static void main(String[] args) {
        JSONObject result=new JSONObject();
        try {
            require(Build.VERSION.SDK_INT==32 && Os.getuid()==0 && Os.getgid()==0,"Wrong fixture caller");
            require(args.length==1 && args[0].matches("[a-f0-9]{32}"),"Invalid profile nonce");
            String nonce=args[0],path="/storage/emulated/0/.pocket-ce32-public-"+nonce;
            String lower="/data/media/0/.pocket-ce32-public-"+nonce;
            byte[] expected=("ce32-public-canary-"+nonce).getBytes("UTF-8");
            JSONObject before=stat(PRIVATE);
            require(before.getInt("mode")==0600 && before.getInt("uid")==0 && before.getInt("gid")==0
                && Arrays.equals(read(PRIVATE),("ce32-canary-"+nonce).getBytes("UTF-8")),"Original private canary differs");
            requirePrivateAliasDenied();
            boolean created=false;long syncMillis=0;
            System.out.println("POCKET_CE32_PUBLIC_PREPARE exclusive-create");System.out.flush();
            // An existing fixture must already contain exactly this profile's
            // bytes. Never truncate, chmod, reseed or follow an existing link.
            try(FileChannel channel=FileChannel.open(Paths.get(path),StandardOpenOption.CREATE_NEW,
                    StandardOpenOption.WRITE,LinkOption.NOFOLLOW_LINKS)) {
                created=true;ByteBuffer data=ByteBuffer.wrap(expected);
                while(data.hasRemaining())channel.write(data);
                long started=SystemClock.elapsedRealtime();channel.force(true);
                syncMillis=SystemClock.elapsedRealtime()-started;
                require(syncMillis<30000,"Public canary fsync exceeded the existing file deadline");
            }catch(FileAlreadyExistsException existing){}
            JSONObject publicStat=stat(path),lowerStat=stat(lower);
            require(publicStat.getInt("uid")>=10000 && publicStat.getInt("uid")==lowerStat.getInt("uid")
                && publicStat.getLong("inode")==lowerStat.getLong("inode")
                && Arrays.equals(read(path),expected) && Arrays.equals(read(lower),expected),"Provider-created canary differs");
            JSONObject after=stat(PRIVATE);
            require(before.toString().equals(after.toString())
                && Arrays.equals(read(PRIVATE),("ce32-canary-"+nonce).getBytes("UTF-8")),"Original private canary changed");
            requirePrivateAliasDenied();
            result.put("schema",1).put("profileNonce",nonce).put("publicPath",path).put("lowerPath",lower)
                .put("createdExclusively",created).put("existingFixtureVerifiedWithoutWrite",!created)
                .put("createdThroughMediaProvider",true).put("publicFsyncMillis",syncMillis)
                .put("publicStat",publicStat).put("lowerStat",lowerStat)
                .put("originalPrivateBefore",before).put("originalPrivateAfter",after)
                .put("originalPrivateBytesPreserved",true).put("originalPrivatePublicAliasDenied",true)
                .put("validation","PASS");
            System.out.println("POCKET_CE32_PUBLIC "+result);System.out.flush();System.exit(0);
        }catch(Throwable error) {
            System.err.println("POCKET_CE32_PUBLIC_ERROR "+error);error.printStackTrace();System.exit(1);
        }
    }
}
