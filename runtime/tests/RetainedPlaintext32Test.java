// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.*;
import java.nio.file.*;
import java.util.*;

public final class RetainedPlaintext32Test {
    private interface Operation {void run()throws Exception;}
    private static void rejected(Operation operation)throws Exception {
        try{operation.run();}catch(IOException expected){return;}
        throw new AssertionError("Unsafe retained migration candidate accepted");
    }
    private static Path profile(Path files)throws Exception {
        Path root=Files.createDirectories(files.resolve("api32"));
        Files.createDirectory(root.resolve("tmp"));
        for(String name:RetainedPlaintext32.ROOTS)Files.createDirectories(root.resolve("system-data/"+name));
        return root;
    }
    public static void main(String[] args)throws Exception {
        Path temp=Files.createTempDirectory("pocket-retained-plaintext-");
        Path files=temp.resolve("files"),root=profile(files);
        byte[] existing="retain-app-key-and-data".getBytes("US-ASCII");
        Path sentinel=root.resolve("system-data/user/0/original-app");Files.write(sentinel,existing);
        RetainedPlaintext32.requireCandidate(root.toFile(),files.toFile());
        for(String name:new String[]{".pocket-ce-enabled",".pocket-ce-enabled.new",".pocket-ce-layout-12l",".pocket-ce-v1", ".pocket-ce-stage-owned", ".pocket-ce-migration-verified", ".pocket-ce-authless.new",".pocket-image-installing"}) {
            Path marker=root.resolve(name);Files.createFile(marker);
            rejected(()->RetainedPlaintext32.requireCandidate(root.toFile(),files.toFile()));Files.delete(marker);
        }
        Path checkpoint=files.resolve("ce32-restart-state-old.json");Files.write(checkpoint,new byte[]{1});
        rejected(()->RetainedPlaintext32.requireCandidate(root.toFile(),files.toFile()));Files.delete(checkpoint);
        Path app=root.resolve("system-data/user/0/"+RetainedPlaintext32.TEST_PACKAGE);Files.createDirectory(app);
        rejected(()->RetainedPlaintext32.requireCandidate(root.toFile(),files.toFile()));Files.delete(app);
        Path observer=root.resolve("tmp/manager-ce-observer.apk");Files.createSymbolicLink(observer,temp.resolve("missing"));
        rejected(()->RetainedPlaintext32.requireCandidate(root.toFile(),files.toFile()));Files.delete(observer);
        Path apk=files.resolve("ce32-test.apk");Files.write(apk,existing);
        rejected(()->RetainedPlaintext32.requireCandidate(root.toFile(),files.toFile()));
        if(!Arrays.equals(existing,Files.readAllBytes(apk)))throw new AssertionError("Existing observer asset changed");Files.delete(apk);
        List<Path> canaries=RetainedPlaintext32.canaries(root.toFile());
        Path collision=canaries.get(4);Files.createSymbolicLink(collision,temp.resolve("missing"));
        rejected(()->RetainedPlaintext32.requireCandidate(root.toFile(),files.toFile()));
        rejected(()->RetainedPlaintext32.seed(root.toFile(),"a".repeat(32)));
        for(int i=0;i<4;i++)if(Files.exists(canaries.get(i)))throw new AssertionError("Partial seed before collision check");
        Files.delete(collision);
        Path vendor=root.resolve("system-data/vendor_ce/0"),outside=Files.createDirectory(temp.resolve("outside"));
        Files.delete(vendor);Files.createSymbolicLink(vendor,outside);
        rejected(()->RetainedPlaintext32.requireCandidate(root.toFile(),files.toFile()));
        Files.delete(vendor);Files.createDirectory(vendor);
        Path tmp=root.resolve("tmp");Files.delete(tmp);Files.createSymbolicLink(tmp,outside);
        rejected(()->RetainedPlaintext32.requireCandidate(root.toFile(),files.toFile()));Files.delete(tmp);Files.createDirectory(tmp);
        Path alias=temp.resolve("alias");Files.createSymbolicLink(alias,root);
        rejected(()->RetainedPlaintext32.requireCandidate(alias.toFile(),files.toFile()));
        rejected(()->RetainedPlaintext32.seed(root.toFile(),"bad-nonce"));
        RetainedPlaintext32.requireCandidate(root.toFile(),files.toFile());
        RetainedPlaintext32.seed(root.toFile(),"a".repeat(32));
        rejected(()->RetainedPlaintext32.seed(root.toFile(),"b".repeat(32)));
        for(Path path:canaries)if(!new String(Files.readAllBytes(path),"US-ASCII").equals("ce32-canary-"+"a".repeat(32)))throw new AssertionError("Canary overwritten");
        if(!Arrays.equals(existing,Files.readAllBytes(sentinel)))throw new AssertionError("Retained app data changed");
        System.out.println("RETAINED_PLAINTEXT_GUARDS_PASS roots=5 exclusive_seeds=1 collisions=1 encrypted_state=1 app_data_preserved=1");
    }
}
