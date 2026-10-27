// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.*;
import java.nio.file.*;
import java.util.Arrays;

/** Prepare our ELF interpreter; never modifies Bionic or the stock linker. */
final class NativeLoader {
    static void prepare(File root)throws Exception {
        File file=new File(root,"system/bin/pocket-loader");
        byte[] bytes=Files.readAllBytes(file.toPath());byte[] magic="PCKELF01".getBytes("US-ASCII");
        int record=-1;
        for(int i=0;i+3080<=bytes.length;i++) {
            boolean match=true;for(int j=0;j<magic.length;j++)if(bytes[i+j]!=magic[j]){match=false;break;}
            if(match){if(record!=-1)throw new IOException("Ambiguous native loader config");record=i+8;}
        }
        if(record<0)throw new IOException("Missing native loader config");
        for(String path:new String[]{root.getPath(),file.getPath(),new File(root,"system/bin/linker64").getPath()}) {
            byte[] value=path.getBytes("UTF-8");if(value.length>=1024)throw new IOException("Native loader path too long");
            Arrays.fill(bytes,record,record+1024,(byte)0);System.arraycopy(value,0,bytes,record,value.length);record+=1024;
        }
        Path next=new File(file.getPath()+".new").toPath();Files.write(next,bytes);android.system.Os.chmod(next.toString(),0755);
        Files.move(next,file.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
    }
}
