// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;

// Standalone JVM harness uses the production JNI symbols without Android APIs.
final class CeFuse {
    static native long start(int fd,Object callbacks);
    static native void stop(long server);
}
public final class CeFuseConcurrencyTest {
    static native int[] pair();
    static native void send(int fd,int opcode,long unique);
    static native long[] receive(int fd,int timeoutMs);
    static native void closeFd(int fd);
    private static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
    public static final class Callbacks {
        final CountDownLatch entered=new CountDownLatch(1),release=new CountDownLatch(1);
        volatile int writes;volatile boolean fail;
        public long fuseSize(int id){return writes;}
        public byte[] fuseRead(int id,long offset,int size){return new byte[Math.min(size,writes)];}
        public int fuseWrite(int id,long offset,byte[] data){writes+=data.length;return data.length;}
        public void fuseOpened(int id){}
        public void fuseRelease(int id){}
        public void fuseSync()throws Exception {entered.countDown();release.await();if(fail)throw new java.io.IOException("Injected sync failure");}
    }
    public static void main(String[] args)throws Exception {
        System.load(args[0]);int[] sockets=pair();Callbacks cb=new Callbacks();long server=CeFuse.start(sockets[0],cb);
        check(server!=0,"server start");
        try {
            send(sockets[1],16,10); // FUSE_WRITE, four bytes
            check(receive(sockets[1],2000)[0]==10,"prior write reply");
            send(sockets[1],20,11); // FUSE_FSYNC
            check(cb.entered.await(2,TimeUnit.SECONDS),"sync did not enter");check(cb.writes==4,"sync overtook prior write");
            send(sockets[1],3,12); // FUSE_GETATTR while sync remains blocked
            long[] reply=receive(sockets[1],2000);
            check(reply[0]==12 && reply[1]==0,"read-only request blocked behind fsync");
            send(sockets[1],16,13);check(receive(sockets[1],2000)[0]==13,"write blocked behind fsync");
            check(receive(sockets[1],30)[0]==-1,"fsync acknowledged before durable callback finished");
            cb.release.countDown();reply=receive(sockets[1],2000);check(reply[0]==11 && reply[1]==0,"sync completion reply");
            cb.fail=true;send(sockets[1],20,14);reply=receive(sockets[1],2000);
            check(reply[0]==14 && reply[1]==-5,"sync error was not propagated");
        } finally {cb.release.countDown();CeFuse.stop(server);closeFd(sockets[1]);}
        sockets=pair();cb=new Callbacks();server=CeFuse.start(sockets[0],cb);
        try {
            send(sockets[1],20,20);check(cb.entered.await(2,TimeUnit.SECONDS),"closing sync did not enter");
            for(int i=0;i<80;i++)send(sockets[1],20,100+i); // exercise full-queue shutdown
            final long handle=server;AtomicReference<Throwable> error=new AtomicReference<>();CountDownLatch closed=new CountDownLatch(1);
            Thread closer=new Thread(()->{try{CeFuse.stop(handle);}catch(Throwable e){error.set(e);}finally{closed.countDown();}},"FuseStopTest");
            closer.setDaemon(true);closer.start();check(!closed.await(50,TimeUnit.MILLISECONDS),"stop freed an in-flight callback");
            cb.release.countDown();check(closed.await(2,TimeUnit.SECONDS),"stop failed to join workers");check(error.get()==null,"stop failure");server=0;
        } finally {cb.release.countDown();if(server!=0)CeFuse.stop(server);closeFd(sockets[1]);}
        System.out.println("PASS: reads/writes during blocked fsync, prior-write ordering, no early durability reply, EIO propagation, shutdown joins in-flight sync");
    }
}
