// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.*;
import java.util.*;

/** Host-only protocol fixture. Its shell is Linux, not a claim of guest root. */
public final class AdminServerFixture {
    public static void main(String[] args) throws Exception {
        final boolean[] running={true};
        final int[] executed={0};
        AdminServer.Backend backend=new AdminServer.Backend(){
            public String status(){return "{\"running\":"+running[0]+",\"executed\":"+executed[0]+"}";}
            public String control(boolean start){running[0]=start;return status();}
            public String resetModuleView() throws IOException {
                if(running[0])throw new IOException("Stop the guest first");
                return "module-view-reset";
            }
            public Process execute(String cwd,List<String> argv)throws Exception {
                executed[0]++;
                List<String> command=new ArrayList<>(argv);
                if(command.get(0).startsWith("/system/bin/"))command.set(0,"/bin/"+command.get(0).substring(12));
                return new ProcessBuilder(command).directory(new File(cwd)).start();
            }
        };
        byte[] key=new byte[32];Arrays.fill(key,(byte)0x11);
        AdminServer server=new AdminServer(0,key,backend);
        System.out.println(server.port());System.out.flush();
        try(BufferedReader input=new BufferedReader(new InputStreamReader(System.in))) {
            String command;
            while((command=input.readLine())!=null) {
                if(command.equals("rotate")) {
                    server.close();Arrays.fill(key,(byte)0x22);server=new AdminServer(0,key,backend);
                    System.out.println(server.port());System.out.flush();
                } else break;
            }
        } finally {server.close();}
    }
}
