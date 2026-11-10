// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import java.io.*;
import java.nio.file.*;
import java.util.*;

/** Checks the kernel-relative view used by 12L's Path.subpath install traversal. */
public final class DirectDataPathFixture {
    private interface Action {void run()throws Exception;}
    private static void rejected(Action action)throws Exception {
        try{action.run();throw new AssertionError("Unexpected data replacement");}catch(IOException expected){}
    }
    public static void main(String[] args)throws Exception {
        Path root=Files.createTempDirectory("pocket-data-path-");
        try {
            Path actual=Files.createDirectories(root.resolve("system-data/app"));
            Path sentinel=actual.resolve("keep");Files.write(sentinel,new byte[]{42});
            Files.createDirectory(root.resolve("data"));
            DirectImage.prepareDataPath(root.toFile(),32);DirectImage.prepareDataPath(root.toFile(),32);
            Process child=new ProcessBuilder("python3","-c",
                "import os; assert os.path.isdir('data/app'); os.mkdir('data/app/~~install'); os.chdir('data'); assert os.path.isdir('../data/app/~~install')")
                .directory(root.toFile()).inheritIO().start();
            if(child.waitFor()!=0 || !Files.isDirectory(actual.resolve("~~install")))throw new AssertionError("Relative and absolute data views disagree");
            DirectImage.prepareDataPath(root.toFile(),29);
            if(Files.isSymbolicLink(root.resolve("data")) || !Files.isDirectory(root.resolve("data")))throw new AssertionError("Legacy data layout changed");
            Files.write(root.resolve("data/foreign"),new byte[]{7});
            rejected(()->DirectImage.prepareDataPath(root.toFile(),32));
            if(Files.readAllBytes(root.resolve("data/foreign"))[0]!=7)throw new AssertionError("Nonempty data directory changed");
            Files.delete(root.resolve("data/foreign"));Files.delete(root.resolve("data"));
            Files.createSymbolicLink(root.resolve("data"),Paths.get("../other-profile"));
            rejected(()->DirectImage.prepareDataPath(root.toFile(),32));
            if(!Files.readSymbolicLink(root.resolve("data")).toString().equals("../other-profile") || Files.readAllBytes(sentinel)[0]!=42)
                throw new AssertionError("Foreign alias or private data changed");
            System.out.println("PASS: 12L relative data traversal, legacy layout, repeat preparation, and data/foreign-link preservation");
        }finally {
            try(java.util.stream.Stream<Path> paths=Files.walk(root)) {
                for(Path path:(Iterable<Path>)paths.sorted(Comparator.reverseOrder())::iterator)Files.deleteIfExists(path);
            }
        }
    }
}
