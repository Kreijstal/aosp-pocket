// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/** Actual child-process IPC checks; the peer supplies no Android functionality. */
public final class KeyguardObserver32Test {
    private static final String RUN="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",NONCE="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    private static Path root,peer;
    private static String python;
    private static int checks;
    private static void check(boolean value,String message){checks++;if(!value)throw new AssertionError(message);}
    private static Path work(String name)throws Exception{return Files.createDirectory(root.resolve(name));}
    private static ProcessBuilder builder(String mode,Path work){return new ProcessBuilder(python,peer.toString(),mode,RUN,NONCE,work.toString());}
    private static KeyguardObserver32 open(String mode,Path work,long timeout)throws Exception {
        return new KeyguardObserver32(builder(mode,work),work.resolve("observer.log").toFile(),RUN,NONCE,timeout);
    }
    private static void reaped(Path work)throws Exception {
        Path file=work.resolve("pid");if(!Files.exists(file))return;
        String pid=new String(Files.readAllBytes(file),StandardCharsets.US_ASCII);
        Path cmd=Paths.get("/proc",pid,"cmdline");
        check(!Files.exists(cmd) || !new String(Files.readAllBytes(cmd),StandardCharsets.UTF_8).contains(peer.toString()),"Owned peer still running: "+pid);
    }
    private static void rejectedPreparation(String mode,long timeout)throws Exception {
        Path work=work(mode);boolean rejected=false;
        try(KeyguardObserver32 observer=open(mode,work,timeout)){}
        catch(IOException expected){rejected=true;}
        check(rejected,"Invalid readiness was accepted: "+mode);reaped(work);
    }
    private static void rejectedResponse(String mode,long timeout)throws Exception {
        Path work=work(mode);KeyguardObserver32 observer=open(mode,work,2000);boolean rejected=false;
        try{observer.query("keyguard",timeout);}catch(IOException expected){rejected=true;}
        finally{observer.close();}
        check(rejected,"Invalid response was accepted: "+mode);check(!observer.closedCleanly(),"Bad exchange claimed clean close");
        check(!observer.isAlive(),"Bad peer was not terminated");reaped(work);
    }
    public static void main(String[] args)throws Exception {
        python=args[0];root=Files.createTempDirectory("keyguard-observer32-");peer=root.resolve("peer.py");
        String source=String.join("\n",
            "import base64,json,os,pathlib,sys,time",
            "mode,run,nonce,directory=sys.argv[1:]",
            "work=pathlib.Path(directory);(work/'pid').write_text(str(os.getpid()))",
            "if mode=='ready-timeout':time.sleep(2)",
            "if mode=='slow-start':time.sleep(.25)",
            "print('ordinary startup stderr',file=sys.stderr,flush=True)",
            "ready_nonce='c'*32 if mode=='wrong-ready' else nonce",
            "ready_pid=0 if mode=='zero-pid' else os.getpid()",
            "print(f'POCKET_CE32_KEYGUARD_READY 1 {run} {ready_nonce} 1000 {ready_pid}',flush=True)",
            "for line in sys.stdin:",
            " with (work/'requests').open('a') as f:f.write(line)",
            " version,r,n,seq,op=line.strip().split(' ')",
            " assert version=='1' and r==run and n==nonce",
            " if mode=='response-timeout':time.sleep(2)",
            " if mode=='exit-response':sys.exit(0)",
            " payload=b'closed' if op=='close' else json.dumps({'operation':op,'interactive':True}).encode()",
            " if mode=='oversize-body':payload=b'x'*65537",
            " encoded=base64.b64encode(payload).decode()",
            " if mode=='bad-base64':encoded='?notbase64?'",
            " response_nonce='c'*32 if mode=='wrong-response' else nonce",
            " response_seq=int(seq)+1 if mode=='future-response' else int(seq)",
            " packet=f'POCKET_CE32_KEYGUARD_RESPONSE 1 {run} {response_nonce} {response_seq} 1000 {os.getpid()} {encoded}\\n'",
            " if mode=='oversize-log':sys.stdout.write('x'*(1024*1024+1)+'\\n')",
            " if mode=='partial-line':",
            "  middle=len(packet)//2;sys.stdout.write(packet[:middle]);sys.stdout.flush();time.sleep(.075);sys.stdout.write(packet[middle:]);sys.stdout.flush()",
            " else:",
            "  sys.stdout.write(packet*2 if mode=='duplicate-response' else packet);sys.stdout.flush()",
            " if op=='close':sys.exit(0)",
            "");
        Files.write(peer,source.getBytes(StandardCharsets.UTF_8));
        Path slow=work("slow-start");KeyguardObserver32 observer=open("slow-start",slow,2000);
        check(observer.startupMillis()>=200,"Constructor returned before cold preparation finished");
        check(!Files.exists(slow.resolve("requests")),"Input was sent before observer readiness");int pid=observer.pid();
        check(observer.query("keyguard",2000).contains("interactive"),"Missing state response");
        check(observer.query("keyguard-ui",2000).contains("keyguard-ui") && observer.pid()==pid,"Warm query replaced its peer");
        check(observer.lastQueryMillis()<1500,"Warm query included another cold startup");observer.close();
        check(observer.closedCleanly() && !observer.isAlive(),"Clean close did not acknowledge/reap peer");reaped(slow);
        Path partial=work("partial-line");try(KeyguardObserver32 reader=open("partial-line",partial,2000)){
            check(reader.query("keyguard",2000).contains("interactive"),"Partial writer record was consumed prematurely");
        }reaped(partial);
        Path unknown=work("unknown-operation");try(KeyguardObserver32 reader=open("normal",unknown,2000)){
            boolean refused=false;try{reader.query("type-password",2000);}catch(IOException expected){refused=true;}
            check(refused && reader.requests()==0 && !Files.exists(unknown.resolve("requests")),"Unsupported operation reached the peer");
            check(reader.query("keyguard",2000).contains("interactive"),"Local rejection poisoned a valid connection");
        }reaped(unknown);
        for(String mode:new String[]{"wrong-ready","zero-pid"})rejectedPreparation(mode,2000);
        rejectedPreparation("ready-timeout",150);
        for(String mode:new String[]{"wrong-response","future-response","duplicate-response","bad-base64","oversize-body","oversize-log","exit-response"})rejectedResponse(mode,2000);
        rejectedResponse("response-timeout",150);
        System.out.println("PASS: "+checks+" observer transport assertions (readiness, warm reuse, partial writes, identity/sequence, bounds, deadlines and child cleanup)");
    }
}
