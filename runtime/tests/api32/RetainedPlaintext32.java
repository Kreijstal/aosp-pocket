// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.*;
import java.nio.channels.FileChannel;
import java.nio.file.*;
import java.util.*;

/** Test-only guards for the existing normal Manager's plaintext profile. */
final class RetainedPlaintext32 {
    static final String PACKAGE="dev.kreijstal.aosppocket.api32managerimage";
    static final String TEST_PACKAGE="dev.kreijstal.pocketce32test";
    static final String[] ROOTS={"user/0","system_ce/0","misc_ce/0","media/0","vendor_ce/0"};
    private static void require(boolean ok,String message)throws IOException {
        if(!ok)throw new IOException(message);
    }
    static List<Path> canaries(File root)throws IOException {
        Path data=root.toPath().resolve("system-data");
        require(Files.isDirectory(data,LinkOption.NOFOLLOW_LINKS),"Missing real plaintext data directory");
        List<Path> paths=new ArrayList<>();
        for(String name:ROOTS) {
            Path current=data;
            for(String part:name.split("/")) {
                current=current.resolve(part);
                require(Files.isDirectory(current,LinkOption.NOFOLLOW_LINKS),"Plaintext CE root must have real directory parents");
            }
            paths.add(current.resolve(".pocket-ce32-canary"));
        }
        return paths;
    }
    static void requireCandidate(File root,File files)throws IOException {
        require(Files.isDirectory(root.toPath(),LinkOption.NOFOLLOW_LINKS)
            && root.toPath().toRealPath().equals(files.toPath().toRealPath().resolve("api32")),"Unexpected retained profile root");
        try(DirectoryStream<Path> entries=Files.newDirectoryStream(root.toPath())) {
            for(Path path:entries) {
                String name=path.getFileName().toString();
                require(!(name.startsWith(".pocket-ce-enabled") || name.startsWith(".pocket-ce-layout-12l")
                    || name.startsWith(".pocket-ce-v1") || name.startsWith(".pocket-ce-stage-")
                    || name.startsWith(".pocket-ce-migration-verified") || name.startsWith(".pocket-ce-authless")
                    || name.equals(".pocket-image-installing")),
                    "Retained profile has encryption state requiring owner recovery");
            }
        }
        try(DirectoryStream<Path> entries=Files.newDirectoryStream(files.toPath(),"ce32-restart-state*")) {
            require(!entries.iterator().hasNext(),"Retained profile already has a credential lifecycle checkpoint");
        }
        for(Path path:canaries(root))require(!Files.exists(path,LinkOption.NOFOLLOW_LINKS),"Retained profile already has a CE test canary");
        Path app=root.toPath().resolve("system-data/user/0/"+TEST_PACKAGE);
        require(!Files.exists(app,LinkOption.NOFOLLOW_LINKS),"Retained profile already has CE test app data");
        require(Files.isDirectory(root.toPath().resolve("tmp"),LinkOption.NOFOLLOW_LINKS),"Retained observer directory must be real");
        require(!Files.exists(files.toPath().resolve("ce32-test.apk"),LinkOption.NOFOLLOW_LINKS)
            && !Files.exists(root.toPath().resolve("tmp/manager-ce-observer.apk"),LinkOption.NOFOLLOW_LINKS),"Retained profile has a previous CE observer artifact");
    }
    static void seed(File root,String nonce)throws IOException {
        require(nonce!=null && nonce.matches("[a-f0-9]{32}"),"Invalid retained canary nonce");
        List<Path> paths=canaries(root);
        // Reject every collision before creating the first owned seed. Keep
        // successfully created seeds if a later write fails; never roll back
        // or truncate an unknown file during recovery.
        for(Path path:paths)require(!Files.exists(path,LinkOption.NOFOLLOW_LINKS),"Retained canary collision");
        byte[] bytes=("ce32-canary-"+nonce).getBytes(java.nio.charset.StandardCharsets.US_ASCII);
        for(Path path:paths) {
            try(FileChannel channel=FileChannel.open(path,StandardOpenOption.CREATE_NEW,StandardOpenOption.WRITE,LinkOption.NOFOLLOW_LINKS)) {
                java.nio.ByteBuffer buffer=java.nio.ByteBuffer.wrap(bytes);
                while(buffer.hasRemaining())channel.write(buffer);
                channel.force(true);
            }
        }
    }
}
