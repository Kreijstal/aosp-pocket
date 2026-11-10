// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.content.*;
import android.content.pm.*;
import android.net.Uri;
import android.os.*;
import android.system.Os;
import dev.kreijstal.aosppocket.bridge.HostWire;
import java.io.*;
import java.lang.reflect.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import org.json.JSONObject;

/** Owner-invoked operations against the actual 12L framework. */
public final class ManagerTools32 {
    private static void check(boolean value,String message)throws IOException {if(!value)throw new IOException(message);}
    private static void launch(Intent intent)throws Exception {
        Object manager=Class.forName("android.app.ActivityTaskManager").getMethod("getService").invoke(null);
        Class<?> api=Class.forName("android.app.IActivityTaskManager");
        Method start=null;
        for(Method candidate:api.getMethods())if(candidate.getName().equals("startActivityAsUser")
                && candidate.getParameterCount()==12 && candidate.getParameterTypes()[3]==Intent.class)start=candidate;
        check(start!=null,"Missing Android 12L activity-service ABI");
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        int status=(Integer)start.invoke(manager,null,"android",null,intent,null,null,null,0,0,null,null,0);
        check(status>=0,"Activity start failed: "+status);
    }
    private static void open(Context context,String name)throws Exception {
        check(name.matches("[a-zA-Z][a-zA-Z0-9_]*(?:\\.[a-zA-Z0-9_]+)+"),"Invalid package name");
        Intent intent=context.getPackageManager().getLaunchIntentForPackage(name);
        check(intent!=null,"Package has no launchable activity: "+name);launch(intent);
    }
    private static String install(Context context,PrintWriter log)throws Exception {
        File directory=new File("/data/pocket-import"),base=new File(directory,"base.apk");
        check(!Files.isSymbolicLink(directory.toPath()) && base.isFile() && base.length()>0,"Imported base APK is missing");
        File[] files=directory.listFiles();check(files!=null && files.length>0 && files.length<=128,"Invalid APK set");
        Arrays.sort(files,Comparator.comparing(File::getName));long total=0;
        for(File file:files) {
            check(file.getName().matches("[a-zA-Z0-9_.-]+\\.apk") && !Files.isSymbolicLink(file.toPath())
                    && file.isFile() && file.length()>0,"Invalid imported APK");
            total=Math.addExact(total,file.length());check(total<=2L*1024*1024*1024,"APK set exceeds size limit");
        }
        PackageInfo info=context.getPackageManager().getPackageArchiveInfo(base.getPath(),0);
        check(info!=null && info.packageName!=null,"Android could not parse the base APK");
        boolean replacing;
        try{context.getPackageManager().getPackageInfo(info.packageName,0);replacing=true;}
        catch(PackageManager.NameNotFoundException absent){replacing=false;}
        PackageInstaller installer=context.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params=new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(info.packageName);params.setSize(total);
        Field flags=params.getClass().getDeclaredField("installFlags");flags.setAccessible(true);flags.setInt(params,flags.getInt(params)|2);
        CountDownLatch delivered=new CountDownLatch(1);Intent[] result={null};
        Binder receiver=new Binder(){
            @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException {
                if(code!=IBinder.FIRST_CALL_TRANSACTION)return super.onTransact(code,data,reply,flags);
                data.enforceInterface("android.content.IIntentSender");data.readInt();
                result[0]=data.readInt()!=0?Intent.CREATOR.createFromParcel(data):null;
                delivered.countDown();return true;
            }
        };
        receiver.attachInterface(null,"android.content.IIntentSender");
        Class<?> sender=Class.forName("android.content.IIntentSender");
        Object target=Class.forName("android.content.IIntentSender$Stub").getMethod("asInterface",IBinder.class).invoke(null,receiver);
        IntentSender callback=IntentSender.class.getConstructor(sender).newInstance(target);
        try(PackageEvents32 events=new PackageEvents32(context,log::println)) {
            long before=events.prepare(info.packageName),deadline=SystemClock.elapsedRealtime()+120000;
            int id=installer.createSession(params);boolean committed=false;
            try {
                try(PackageInstaller.Session session=installer.openSession(id)) {
                    for(File file:files)try(InputStream in=new FileInputStream(file);OutputStream out=session.openWrite(file.getName(),0,file.length())) {
                        byte[] buffer=new byte[65536];int n;while((n=in.read(buffer))!=-1)out.write(buffer,0,n);session.fsync(out);
                    }
                    session.commit(callback);committed=true;
                }
                check(delivered.await(Math.max(0,Math.min(90000,deadline-SystemClock.elapsedRealtime())),TimeUnit.MILLISECONDS)
                    && result[0]!=null,"PackageInstaller did not return a result");
                check(result[0].getIntExtra(PackageInstaller.EXTRA_STATUS,-1)==PackageInstaller.STATUS_SUCCESS,
                    "Install failed: "+result[0].getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
                PackageInfo installed=context.getPackageManager().getPackageInfo(info.packageName,0);
                check(installed.applicationInfo.uid>=10000 && installed.getLongVersionCode()==info.getLongVersionCode(),"Installed package metadata mismatch");
                events.await(installed,before,replacing,deadline);
                Intent intent=context.getPackageManager().getLaunchIntentForPackage(info.packageName);if(intent!=null)launch(intent);
                return "Installed "+info.packageName;
            }finally{if(!committed)installer.abandonSession(id);}
        }
    }
    private static JSONObject network(Context context)throws Exception {
        android.net.ConnectivityManager cm=context.getSystemService(android.net.ConnectivityManager.class);
        android.net.NetworkInfo active=cm.getActiveNetworkInfo();
        android.net.Network network=cm.getActiveNetwork();
        android.net.NetworkCapabilities capabilities=network==null?null:cm.getNetworkCapabilities(network);
        JSONObject result=HostDevices.query(HostWire.INFO,new JSONObject());
        result.put("guest_network_connected",active!=null && active.isConnected());
        result.put("guest_default_network",network==null?JSONObject.NULL:network.toString())
            .put("guest_internet",capabilities!=null && capabilities.hasCapability(android.net.NetworkCapabilities.NET_CAPABILITY_INTERNET))
            .put("guest_validated",capabilities!=null && capabilities.hasCapability(android.net.NetworkCapabilities.NET_CAPABILITY_VALIDATED))
            .put("http_via_guest_network",network!=null);
        java.net.URL url=new java.net.URL("https://example.com/");
        java.net.HttpURLConnection connection=(java.net.HttpURLConnection)(network==null?url.openConnection():network.openConnection(url));
        connection.setConnectTimeout(10000);connection.setReadTimeout(10000);
        try{result.put("http_status",connection.getResponseCode());}
        catch(Exception error){result.put("http_error",error.toString());}
        finally{connection.disconnect();}
        if(network!=null)try(PackageEvents32 services=new PackageEvents32(context)) {
            result.put("guest_async_dns",new JSONObject(services.dns()));
        }catch(Exception error){result.put("guest_async_dns_error",error.toString());}
        return result;
    }
    /** Called only after the private control channel authenticates the host owner. */
    public static String executeOwnerCommand(Context context,String[] args,PrintWriter log)throws Exception {
        return execute(context,args,log,true);
    }
    private static String execute(Context context,String[] args,PrintWriter log,boolean embedded)throws Exception {
        check(Build.VERSION.SDK_INT==32 && Os.getuid()==1000 && context!=null && args.length>=1 && args.length<=2,"Expected SDK32 system-role command");
        switch(args[0]) {
            case "startup":check(embedded && args.length==1 && StartupReadiness32.selected(),"Startup state requires the matching system-server owner channel");return StartupReadiness32.state(context).toString();
            case "install":check(args.length==1,"Unexpected install argument");if(embedded && StartupReadiness32.selected())StartupReadiness32.await(context,log);return install(context,log);
            case "open":check(args.length==2,"Package name required");check(args[1].matches("[a-zA-Z][a-zA-Z0-9_]*(?:\\.[a-zA-Z0-9_]+)+"),"Invalid package name");if(embedded && StartupReadiness32.selected())StartupReadiness32.await(context,log);open(context,args[1]);return "Opened "+args[1];
            case "dialer":check(args.length==1,"Unexpected dialer argument");if(embedded && StartupReadiness32.selected())StartupReadiness32.await(context,log);launch(new Intent(Intent.ACTION_DIAL,Uri.parse("tel:")));return "Dialer opened";
            case "network":check(args.length==1,"Unexpected network argument");return network(context).toString();
            case "contacts":check(args.length==1,"Unexpected contacts argument");if(embedded && StartupReadiness32.selected())StartupReadiness32.await(context,log);return HostDevices.importContacts(context);
            case "devices":check(args.length==1,"Unexpected devices argument");return HostDevices.diagnose(context).toString();
            default:throw new IOException("Unknown Manager operation");
        }
    }
    public static void main(String[] args) {
        try {
            check(Build.VERSION.SDK_INT==32 && Os.getuid()==0 && args.length>=1 && args.length<=2,"Expected SDK32 owner entry");
            Os.setgid(1000);Os.setuid(1000);Looper.prepare();
            Class<?> api=Class.forName("android.app.ActivityThread");Object thread=api.getMethod("systemMain").invoke(null);
            Context context=(Context)api.getMethod("getSystemContext").invoke(thread);
            // The standalone compatibility CLI has no local AMS object. Its
            // established Binder path stays available; normal Manager images
            // use the authenticated system-server entry above.
            String result=execute(context,args,new PrintWriter(System.out,true),false);
            System.out.println("POCKET_MANAGER32_RESULT "+result);System.out.flush();System.exit(0);
        }catch(Throwable error) {
            while(error instanceof InvocationTargetException && error.getCause()!=null)error=error.getCause();
            System.err.println("POCKET_MANAGER32_ERROR "+error);error.printStackTrace(System.err);System.exit(1);
        }
    }
}
