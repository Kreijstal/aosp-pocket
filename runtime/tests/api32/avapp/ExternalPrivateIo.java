// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketav32test;

import android.content.Context;
import android.os.Environment;
import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.util.Arrays;
import org.json.JSONObject;

/** Actual ordinary-app private emulated storage I/O, retained across Home. */
final class ExternalPrivateIo {
    private static File previousFile;
    private static byte[] previousBytes;
    private static String previousNonce;
    private static String digest(byte[] bytes)throws Exception {
        StringBuilder out=new StringBuilder();
        for(byte value:MessageDigest.getInstance("SHA-256").digest(bytes))out.append(String.format(java.util.Locale.ROOT,"%02x",value&255));
        return out.toString();
    }
    static JSONObject run(Context context,String nonce)throws Exception {
        File directory=context.getExternalFilesDir(null);
        if(directory==null || !directory.isDirectory() || !Environment.MEDIA_MOUNTED.equals(Environment.getExternalStorageState(directory)))
            throw new IOException("Private emulated directory is not mounted");
        String prefix="/storage/emulated/0/Android/data/"+context.getPackageName()+"/files/";
        JSONObject result=new JSONObject().put("uid",android.os.Process.myUid());
        if(previousFile!=null) {
            byte[] observed=Files.readAllBytes(previousFile.toPath());
            if(!Arrays.equals(observed,previousBytes))throw new IOException("External witness changed across Home");
            result.put("previousWitness",new JSONObject().put("nonce",previousNonce).put("path",previousFile.getCanonicalPath())
                .put("bytes",observed.length).put("sha256",digest(observed)));
        }
        byte[] expected=("Pocket AV external "+nonce+"\n").getBytes(StandardCharsets.UTF_8);
        File file=new File(directory,"pocket-av-"+nonce);
        try(FileChannel channel=FileChannel.open(file.toPath(),StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE)) {
            ByteBuffer bytes=ByteBuffer.wrap(expected);
            while(bytes.hasRemaining())if(channel.write(bytes)<=0)throw new IOException("External write made no progress");
            channel.force(true);
        }
        byte[] observed=Files.readAllBytes(file.toPath());
        if(!Arrays.equals(observed,expected) || Files.size(file.toPath())!=expected.length)throw new IOException("External file readback differs");
        String path=file.getCanonicalPath();
        if(!path.equals(prefix+"pocket-av-"+nonce))throw new IOException("External witness escaped its package directory");
        previousFile=file;previousBytes=expected;previousNonce=nonce;
        return result.put("path",path).put("nonce",nonce).put("bytes",observed.length).put("sha256",digest(observed))
            .put("exclusiveCreate",true).put("syncCompleted",true).put("readbackVerified",true);
    }
}
