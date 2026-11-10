// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;
import android.net.*;
import android.os.*;
import android.system.Os;
import android.view.*;
import java.io.*;
import java.lang.reflect.*;
import java.nio.file.*;
import java.util.concurrent.Semaphore;

/** Private host-owner control channel to the actual SDK 32 framework. */
public final class InputControl32 {
    private static int hostPid,kernelUid,width,height;
    private static String hostStart;
    private static Object manager;
    private static Method inject;
    private static final Semaphore clients=new Semaphore(4);
    private static final Semaphore tools=new Semaphore(1);
    private static volatile Method ownerCommand;
    private static Method ownerCommand()throws Exception {
        Method method=ownerCommand;
        if(method==null)synchronized(InputControl32.class) {
            method=ownerCommand;
            if(method==null) {
                ClassLoader loader=new dalvik.system.PathClassLoader("/system/framework/pocket-manager.jar",InputControl32.class.getClassLoader());
                method=Class.forName("dev.kreijstal.aosppocket.guest.ManagerTools32",true,loader)
                    .getMethod("executeOwnerCommand",android.content.Context.class,String[].class,PrintWriter.class);
                ownerCommand=method;
            }
        }
        return method;
    }
    private static final class ToolOutput extends Writer {
        final StringBuilder text=new StringBuilder();
        @Override public void write(char[] chars,int offset,int count)throws IOException {
            if(count>16384-text.length())throw new IOException("Manager output exceeds limit");
            text.append(chars,offset,count);
        }
        @Override public void flush(){}
        @Override public void close(){}
    }
    private static void tool(android.content.Context context,DataInputStream in,DataOutputStream out)throws Exception {
        int count=in.readInt();if(count<1 || count>2)throw new IOException("Invalid Manager argument count");
        String[] args=new String[count];
        for(int n=0;n<count;n++) {
            int bytes=in.readInt();if(bytes<1 || bytes>1024)throw new IOException("Invalid Manager argument size");
            byte[] value=new byte[bytes];in.readFully(value);args[n]=new String(value,"UTF-8");
        }
        ToolOutput output=new ToolOutput();PrintWriter log=new PrintWriter(output);int status=1;boolean acquired=false;
        try {
            if(context==null)throw new IOException("Manager context is unavailable");
            acquired=tools.tryAcquire();if(!acquired)throw new IOException("Another Manager operation is active");
            String result=(String)ownerCommand().invoke(null,context,args,log);
            log.println("POCKET_MANAGER32_RESULT "+result);log.flush();
            if(log.checkError())throw new IOException("Manager output exceeds limit");status=0;
            System.out.println("API32_MANAGER_CONTROL_COMPLETE operation="+args[0]+" pid="+Os.getpid());
        }catch(Throwable error) {
            while(error instanceof InvocationTargetException && error.getCause()!=null)error=error.getCause();
            System.err.println("API32_MANAGER_CONTROL_ERROR "+error);
            String message=error.toString();if(message.length()>4096)message=message.substring(0,4096);
            output.text.setLength(0);output.text.append("POCKET_MANAGER32_ERROR ").append(message).append('\n');
        }finally{if(acquired)tools.release();}
        byte[] bytes=output.text.toString().getBytes("UTF-8");if(bytes.length>65536)throw new IOException("Manager response exceeds limit");
        out.writeInt(status);out.writeInt(bytes.length);out.write(bytes);out.flush();
    }
    private static void verifyHostLifetime(LocalSocket socket,DataInputStream in)throws Exception {
        // A non-debuggable Android parent need not be visible in its child's
        // proc view. Read the parent's already-open stat descriptor instead.
        // Its kernel fd link prevents substituting a regular file with copied
        // contents; the start time rejects a later reuse of the same PID.
        if(in.read()!=0)throw new IOException("Missing host lifetime descriptor");
        FileDescriptor[] descriptors=socket.getAncillaryFileDescriptors();
        if(descriptors==null || descriptors.length!=1) {
            if(descriptors!=null)for(FileDescriptor fd:descriptors)Os.close(fd);
            throw new IOException("Expected one host lifetime descriptor");
        }
        try(BufferedReader reader=new BufferedReader(new InputStreamReader(new FileInputStream(descriptors[0]),"UTF-8"))) {
            Field number=FileDescriptor.class.getDeclaredField("descriptor");number.setAccessible(true);
            if(!("/proc/"+hostPid+"/stat").equals(Os.readlink("/proc/self/fd/"+number.getInt(descriptors[0]))))
                throw new IOException("Host lifetime descriptor is not its kernel stat file");
            String value=reader.readLine();
            if(value==null || value.length()>8192 || !value.startsWith(hostPid+" (") ||
                !hostStart.equals(value.substring(value.lastIndexOf(')')+2).trim().split(" +")[19]))
                throw new IOException("Host lifetime mismatch");
        }
    }
    private static void serve(LocalSocket peer,android.content.Context context) {
        try(LocalSocket socket=peer) {
            Credentials owner=socket.getPeerCredentials();
            if(owner.getPid()!=hostPid || owner.getUid()!=kernelUid) {
                System.out.println("API32_INPUT_PEER_DENIED pid="+owner.getPid());return;
            }
            socket.setSoTimeout(0);
            DataInputStream in=new DataInputStream(new BufferedInputStream(socket.getInputStream()));
            DataOutputStream out=new DataOutputStream(new BufferedOutputStream(socket.getOutputStream()));
            out.writeInt(0x504b4932);out.writeInt(width);out.writeInt(height);out.flush();
            verifyHostLifetime(socket,in);out.writeBoolean(true);out.flush();
            for(;;) {
                int operation=in.readInt();
                if(operation==4){tool(context,in,out);continue;}
                if(operation==3) {
                    long down=in.readLong(),time=in.readLong();int action=in.readInt(),code=in.readInt(),repeat=in.readInt(),meta=in.readInt();
                    if(down<0 || time<down || (action!=KeyEvent.ACTION_DOWN && action!=KeyEvent.ACTION_UP) ||
                        code<0 || code>KeyEvent.getMaxKeyCode() || repeat<0 || repeat>1024)
                        throw new IOException("Invalid keyboard packet");
                    KeyEvent event=new KeyEvent(down,time,action,code,repeat,meta,KeyCharacterMap.VIRTUAL_KEYBOARD,0,
                        KeyEvent.FLAG_FROM_SYSTEM|KeyEvent.FLAG_KEEP_TOUCH_MODE,InputDevice.SOURCE_KEYBOARD);
                    out.writeBoolean((Boolean)inject.invoke(manager,event,0));out.flush();
                    continue;
                }
                if(operation!=2)throw new IOException("Unknown input operation");
                long down=in.readLong(),time=in.readLong();int action=in.readInt(),count=in.readInt();
                if(count<1 || count>10 || (action&255)>MotionEvent.ACTION_POINTER_UP || action<0 ||
                    (action>>8)>=count || down<0 || time<down)throw new IOException("Invalid touch packet");
                MotionEvent.PointerProperties[] properties=new MotionEvent.PointerProperties[count];
                MotionEvent.PointerCoords[] coordinates=new MotionEvent.PointerCoords[count];int ids=0;
                for(int n=0;n<count;n++) {
                    int id=in.readInt();float x=in.readFloat(),y=in.readFloat();
                    if(id<0 || id>31 || (ids&(1<<id))!=0 || !Float.isFinite(x) || !Float.isFinite(y))throw new IOException("Invalid pointer");
                    ids|=1<<id;properties[n]=new MotionEvent.PointerProperties();properties[n].id=id;
                    properties[n].toolType=MotionEvent.TOOL_TYPE_FINGER;coordinates[n]=new MotionEvent.PointerCoords();
                    coordinates[n].x=Math.max(0,Math.min(width-1,x));coordinates[n].y=Math.max(0,Math.min(height-1,y));
                    coordinates[n].pressure=1;coordinates[n].size=1;
                }
                MotionEvent event=MotionEvent.obtain(down,time,action,count,properties,coordinates,0,0,1,1,0,0,InputDevice.SOURCE_TOUCHSCREEN,0);
                try {
                    boolean accepted=(Boolean)inject.invoke(manager,event,0);
                    System.out.println("API32_HOST_TOUCH_QUEUED action="+action+" x="+coordinates[0].x+" y="+coordinates[0].y+
                        " event_time="+time+" guest_uptime="+SystemClock.uptimeMillis()+" queued="+accepted);
                    out.writeBoolean(accepted);out.flush();
                }finally{event.recycle();}
            }
        }catch(EOFException closed){/* Normal disconnect. */}
        catch(Throwable error){System.err.println("API32_INPUT_CLIENT_ERROR "+error);}
        finally{clients.release();}
    }
    private static void initialize()throws Exception {
        if(Build.VERSION.SDK_INT!=32 || Os.getuid()!=1000)throw new AssertionError("Expected SDK 32 system role");
        hostPid=Integer.parseInt(System.getenv("POCKET_API32_HOST_PID"));hostStart=System.getenv("POCKET_API32_HOST_START");
        if(hostPid<=0 || hostStart==null || !hostStart.matches("[0-9]{1,20}"))throw new AssertionError("Invalid host lifetime");
        String status=new String(Files.readAllBytes(Paths.get("/proc/self/status")),"UTF-8");
        java.util.regex.Matcher uid=java.util.regex.Pattern.compile("(?m)^Uid:\\s+(\\d+)").matcher(status);
        if(!uid.find())throw new AssertionError("No kernel UID");kernelUid=Integer.parseInt(uid.group(1));
        Class<?> properties=Class.forName("android.os.SystemProperties");
        width=(Integer)properties.getMethod("getInt",String.class,int.class).invoke(null,"ro.boot.redroid_width",480);
        height=(Integer)properties.getMethod("getInt",String.class,int.class).invoke(null,"ro.boot.redroid_height",800);
        if(width<1 || height<1 || width>8192 || height>8192)throw new AssertionError("Invalid geometry");
        IBinder binder=(IBinder)Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,"input");
        if(binder==null)throw new AssertionError("InputManager missing");
        manager=Class.forName("android.hardware.input.IInputManager$Stub").getMethod("asInterface",IBinder.class).invoke(null,binder);
        inject=Class.forName("android.hardware.input.IInputManager").getMethod("injectInputEvent",InputEvent.class,int.class);
    }
    private static void run(android.content.Context context,LocalServerSocket server,boolean embedded)throws Exception {
        try{if(context!=null)new HostNetwork32(context);}
        catch(Exception | LinkageError network) {
            // Network bootstrap errors must not remove the input channel.
            System.err.println("API32_HOST_NETWORK_START_FAILED "+network);
        }
        String ready="API32_INPUT_CONTROL_READY sdk=32 host_pid="+hostPid+" kernel_peer_credentials=required"+
            " owner="+(embedded?"system_server":"standalone")+" pid="+Os.getpid();
        if(embedded) {
            // ZygoteInit redirects Java streams to liblog. Boot supervision
            // reads the inherited console file, so publish there as well.
            String log=System.getenv("POCKET_API32_BOOT_LOG");
            if(log==null)log="/logs/manager-init.log";
            if(!log.equals("/logs/manager-init.log") && !log.equals("/tmp/pocket-runtime-validation.log"))
                throw new IOException("Unexpected runtime console");
            try(FileOutputStream output=new FileOutputStream(log,true)){output.write((ready+"\n").getBytes("UTF-8"));}
        }
        System.out.println(ready);
        for(;;) {
            LocalSocket peer=server.accept();if(!clients.tryAcquire()){peer.close();continue;}
            Thread worker=new Thread(()->serve(peer,context),"Api32HostInput");worker.setDaemon(true);worker.start();
        }
    }
    private static final java.util.concurrent.atomic.AtomicBoolean embeddedStarted=new java.util.concurrent.atomic.AtomicBoolean();
    /** Called exclusively in the system-server child, after ART and Binder init. */
    public static void startInSystemServer() {
        if(Build.VERSION.SDK_INT!=32 || Os.getuid()!=1000 || !embeddedStarted.compareAndSet(false,true))
            throw new IllegalStateException("Invalid or duplicate system-server bridge start");
        Thread bridge=new Thread(()->{
            try {
                Class<?> properties=Class.forName("android.os.SystemProperties");
                Method get=properties.getMethod("get",String.class);
                Class<?> activityThread=Class.forName("android.app.ActivityThread");
                long deadline=SystemClock.uptimeMillis()+180000;
                android.content.Context context=null;
                while(context==null) {
                    Object activity=activityThread.getMethod("currentActivityThread").invoke(null);
                    if(activity!=null && "1".equals(get.invoke(null,"sys.boot_completed")))
                        context=(android.content.Context)activityThread.getMethod("getSystemContext").invoke(activity);
                    if(SystemClock.uptimeMillis()>=deadline)throw new IOException("System-server context did not become ready");
                    if(context==null)Thread.sleep(50);
                }
                initialize();
                String path="/dev/socket/pocket_control32";
                try {
                    if(!android.system.OsConstants.S_ISSOCK(Os.lstat(path).st_mode))throw new IOException("Unexpected control socket entry");
                    Files.delete(Paths.get(path));
                }catch(android.system.ErrnoException absent){if(absent.errno!=android.system.OsConstants.ENOENT)throw absent;}
                try(LocalSocket binding=new LocalSocket()) {
                    binding.bind(new LocalSocketAddress(path,LocalSocketAddress.Namespace.FILESYSTEM));
                    Os.chmod(path,0600);
                    try(LocalServerSocket server=new LocalServerSocket(binding.getFileDescriptor())){run(context,server,true);}
                }
            }catch(Throwable error) {
                error.printStackTrace();System.err.println("API32_SYSTEM_SERVER_BRIDGE_FAILED "+error);
                try{Class.forName("android.os.SystemProperties").getMethod("set",String.class,String.class)
                    .invoke(null,"pocket.control.stop","1");}catch(Exception stop){stop.printStackTrace();}
            }
        },"PocketSystemServerBridge32");
        bridge.setDaemon(true);bridge.start();
    }
    public static void main(String[] args) {
        try {
            if(Build.VERSION.SDK_INT!=32 || Os.getuid()!=0)throw new AssertionError("Expected SDK 32 root entry");
            Os.setgid(1000);Os.setuid(1000);initialize();
            android.content.Context context=null;
            try {
                if(Looper.myLooper()==null)Looper.prepare();
                Class<?> activityThread=Class.forName("android.app.ActivityThread");
                Object activity=activityThread.getMethod("systemMain").invoke(null);
                context=(android.content.Context)activityThread.getMethod("getSystemContext").invoke(activity);
            }catch(Exception | LinkageError network){System.err.println("API32_HOST_NETWORK_START_FAILED "+network);}
            FileDescriptor fd=new FileDescriptor();Field descriptor=FileDescriptor.class.getDeclaredField("descriptor");descriptor.setAccessible(true);
            descriptor.setInt(fd,Integer.parseInt(System.getenv("ANDROID_SOCKET_pocket_control32")));
            try(LocalServerSocket server=new LocalServerSocket(fd)){run(context,server,false);}
        }catch(Throwable error){error.printStackTrace();System.exit(1);}
    }
}
