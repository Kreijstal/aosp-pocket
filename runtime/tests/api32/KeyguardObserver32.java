// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.Base64;
import java.util.concurrent.TimeUnit;

/** Test-only read-only observer transport, prepared before physical host input. */
final class KeyguardObserver32 implements AutoCloseable {
    private static final String READY="POCKET_CE32_KEYGUARD_READY";
    private static final String RESPONSE="POCKET_CE32_KEYGUARD_RESPONSE";
    private final String runId,nonce;
    private final File log;
    private final Process process;
    private final long startupMillis;
    private int pid,sequence;
    private long lastQueryMillis;
    private boolean healthy=true,closed,closedCleanly;

    KeyguardObserver32(ProcessBuilder builder,File log,String runId,String nonce,long timeoutMillis)throws IOException,InterruptedException {
        require(runId!=null && runId.matches("[a-f0-9]{32}") && nonce!=null && nonce.matches("[a-f0-9]{32}"),"Invalid observer identity");
        long deadline=deadline(timeoutMillis),started=System.nanoTime();
        this.log=log;this.runId=runId;this.nonce=nonce;
        Files.createFile(log.toPath());
        process=builder.redirectErrorStream(true).redirectOutput(ProcessBuilder.Redirect.appendTo(log)).start();
        try {
            while(true) {
                boolean ready=false;
                for(String line:lines()) {
                    if(!line.startsWith(READY+" "))continue;
                    String[] fields=line.split(" ");
                    require(fields.length==6 && fields[1].equals("1") && fields[2].equals(runId)
                        && fields[3].equals(nonce) && fields[4].equals("1000"),"Wrong observer readiness identity");
                    int peer=positive(fields[5]);require(!ready,"Duplicate observer readiness");pid=peer;ready=true;
                }
                if(ready){require(process.isAlive(),"Observer exited after readiness");break;}
                require(process.isAlive(),"Observer exited before readiness");
                require(System.nanoTime()<deadline,"Observer preparation timed out");Thread.sleep(25);
            }
        }catch(IOException|InterruptedException error) {
            healthy=false;terminate();throw error;
        }
        startupMillis=TimeUnit.NANOSECONDS.toMillis(System.nanoTime()-started);
    }
    private static void require(boolean value,String message)throws IOException {if(!value)throw new IOException(message);}
    private static int positive(String value)throws IOException {
        try{int n=Integer.parseInt(value);require(n>0,"Invalid positive observer field");return n;}
        catch(NumberFormatException error){throw new IOException("Invalid numeric observer field",error);}
    }
    private static long deadline(long timeoutMillis)throws IOException {
        require(timeoutMillis>0 && timeoutMillis<=45000,"Invalid observer operation deadline");
        return System.nanoTime()+TimeUnit.MILLISECONDS.toNanos(timeoutMillis);
    }
    private String[] lines()throws IOException {
        require(Files.isRegularFile(log.toPath(),LinkOption.NOFOLLOW_LINKS) && log.length()<=1024*1024,"Observer output exceeds its bound");
        byte[] bytes=Files.readAllBytes(log.toPath());require(bytes.length<=1024*1024,"Observer output grew beyond its bound");
        String text=new String(bytes,StandardCharsets.UTF_8);
        // A concurrent writer can leave a partial final line. Only completed
        // records are eligible; stderr before readiness is ordinary startup.
        int end=text.lastIndexOf('\n');return end<0?new String[0]:text.substring(0,end).split("\n");
    }
    synchronized String query(String operation,long timeoutMillis)throws IOException,InterruptedException {
        require(operation.equals("keyguard") || operation.equals("keyguard-ui") || operation.equals("keyguard-diagnostic")
            || operation.equals("keyguard-power") || operation.equals("keyguard-await"),"Unsupported read-only observer operation");
        return exchange(operation,timeoutMillis);
    }
    private String exchange(String operation,long timeoutMillis)throws IOException,InterruptedException {
        long until=deadline(timeoutMillis),started=System.nanoTime();
        require(!closed && healthy && process.isAlive(),"Observer is unavailable");
        require(sequence<256,"Observer request limit reached");int wanted=++sequence;
        try {
            String request="1 "+runId+" "+nonce+" "+wanted+" "+operation+"\n";
            process.getOutputStream().write(request.getBytes(StandardCharsets.US_ASCII));process.getOutputStream().flush();
            while(true) {
                String found=null;
                for(String line:lines()) {
                    if(!line.startsWith(RESPONSE+" "))continue;
                    String[] fields=line.split(" ",8);
                    require(fields.length==8 && fields[1].equals("1") && fields[2].equals(runId) && fields[3].equals(nonce)
                        && fields[5].equals("1000") && positive(fields[6])==pid,"Wrong observer response identity");
                    int received=positive(fields[4]);require(received<=wanted,"Unrequested future observer response");
                    if(received<wanted)continue;
                    require(found==null,"Duplicate observer response");
                    byte[] payload;
                    try{payload=Base64.getDecoder().decode(fields[7]);}
                    catch(IllegalArgumentException error){throw new IOException("Invalid observer payload encoding",error);}
                    require(payload.length<=65536,"Observer response exceeds its bound");
                    found=new String(payload,StandardCharsets.UTF_8);
                }
                if(found!=null) {
                    require(System.nanoTime()<=until,"Observer response arrived after its deadline");
                    lastQueryMillis=TimeUnit.NANOSECONDS.toMillis(System.nanoTime()-started);return found;
                }
                require(process.isAlive(),"Observer exited without responding");
                require(System.nanoTime()<until,"Observer request timed out");Thread.sleep(25);
            }
        }catch(IOException|InterruptedException error){healthy=false;throw error;}
    }
    int pid(){return pid;}
    String nonce(){return nonce;}
    int requests(){return sequence;}
    long startupMillis(){return startupMillis;}
    long lastQueryMillis(){return lastQueryMillis;}
    boolean closedCleanly(){return closedCleanly;}
    boolean isAlive(){return process.isAlive();}
    private void terminate()throws InterruptedException,IOException {
        try{process.getOutputStream().close();}catch(IOException ignored){}
        if(process.isAlive())process.destroyForcibly();
        require(process.waitFor(5,TimeUnit.SECONDS),"Observer did not exit after termination");
    }
    @Override public synchronized void close()throws IOException,InterruptedException {
        if(closed)return;
        try {
            if(healthy) {
                require(exchange("close",5000).equals("closed"),"Missing observer close acknowledgement");
                process.getOutputStream().close();
                require(process.waitFor(5,TimeUnit.SECONDS) && process.exitValue()==0,"Observer did not exit cleanly");
                closedCleanly=true;
            }
        }finally{try{terminate();}finally{closed=true;}}
    }
}
