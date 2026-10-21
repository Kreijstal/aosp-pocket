// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import java.nio.file.*;

/** Exercise the production cleanup without starting an Android framework. */
public final class DirectImageLinksFixture {
    public static void main(String[] args) throws Exception {
        Path root=Files.createTempDirectory("pocket-links-");
        Path bin=Files.createDirectories(root.resolve("system/bin"));
        Path getprop=bin.resolve("getprop"),setprop=bin.resolve("setprop");
        try {
            Files.createSymbolicLink(getprop,Paths.get("/system/etc/pocket-magisk/getprop"));
            Files.createSymbolicLink(setprop,Paths.get("/system/bin/toybox"));
            for(String name:new String[]{"magisk","su","resetprop"})
                Files.createSymbolicLink(bin.resolve(name),Paths.get("/sbin/magisk"));
            DirectImage.clearGeneratedLinks(root.toFile());
            for(String name:new String[]{"magisk","su","resetprop"})
                if(Files.exists(bin.resolve(name),LinkOption.NOFOLLOW_LINKS))
                    throw new AssertionError("Generated Magisk link retained: "+name);
            if(Files.exists(getprop,LinkOption.NOFOLLOW_LINKS) || Files.exists(setprop,LinkOption.NOFOLLOW_LINKS))
                throw new AssertionError("Generated links were retained");
            Files.createSymbolicLink(getprop,Paths.get("/unexpected-target"));
            Files.write(setprop,new byte[]{42});
            DirectImage.clearGeneratedLinks(root.toFile());
            if(!Files.isSymbolicLink(getprop) || Files.readAllBytes(setprop)[0]!=42)
                throw new AssertionError("Unrelated link or regular file changed");
            System.out.println("PASS: generated links cleared before preparation; unrelated paths preserved");
        } finally {
            Files.deleteIfExists(getprop);Files.deleteIfExists(setprop);
            Files.delete(bin);Files.delete(root.resolve("system"));Files.delete(root);
        }
    }
}
