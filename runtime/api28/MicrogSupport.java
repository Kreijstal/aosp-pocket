// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.pm.SigningInfo;
import android.content.pm.VersionedPackage;
import android.os.*;
import java.io.*;
import java.lang.reflect.*;
import java.security.MessageDigest;
import java.util.*;

/** API-28 package query compatibility for original, officially signed microG. */
public final class MicrogSupport {
    private static final String DESCRIPTOR="android.content.pm.IPackageManager";
    private static final String MICROG_CERT="9bd06727e62796c0130eb6dab39b73157451582cbd138e86c468acc395d14165";
    private static final int SIGNATURE_FLAGS=PackageManager.GET_SIGNATURES|PackageManager.GET_SIGNING_CERTIFICATES;
    private static boolean installed;
    private MicrogSupport(){}

    /** Called synchronously in system_server after ART's post-fork hooks. */
    public static synchronized void install()throws Exception {
        if(installed)return;
        if(android.os.Process.myUid()!=1000)throw new SecurityException("System-server role required");
        Class<?> manager=Class.forName("android.os.ServiceManager");
        Method get=manager.getDeclaredMethod("getIServiceManager");get.setAccessible(true);
        final Object original=get.invoke(null);
        Class<?> type=Class.forName("android.os.IServiceManager");
        final java.util.concurrent.atomic.AtomicReference<IBinder> localPackage=new java.util.concurrent.atomic.AtomicReference<>();
        final java.util.concurrent.atomic.AtomicReference<PackageQueries> publicPackage=new java.util.concurrent.atomic.AtomicReference<>();
        final java.util.concurrent.atomic.AtomicReference<IBinder> localActivity=new java.util.concurrent.atomic.AtomicReference<>();
        Object proxy=Proxy.newProxyInstance(type.getClassLoader(),new Class<?>[]{type},(ignored,method,args)->{
            // Platform services cast the local binder to PackageManagerService.
            // This Java singleton exists only in system_server; native service
            // manager clients in app processes still receive the public facade.
            if((method.getName().equals("getService")||method.getName().equals("checkService")) && args!=null && args.length>0 && "package".equals(args[0])) {
                IBinder local=localPackage.get();if(local!=null)return local;
            }
            if((method.getName().equals("getService")||method.getName().equals("checkService")) && args!=null && args.length>0 && "activity".equals(args[0])) {
                IBinder local=localActivity.get();if(local!=null)return local;
            }
            if(method.getName().equals("addService") && args!=null && args.length>=2 && "package".equals(args[0]) && args[1] instanceof IBinder && !(args[1] instanceof PackageQueries)) {
                localPackage.set((IBinder)args[1]);
                PackageQueries facade=new PackageQueries((IBinder)args[1]);publicPackage.set(facade);
                args=args.clone();args[1]=facade;
                System.err.println("POCKET_MICROG_PACKAGE_READY scope=official-certificate read_queries=1");
            }
            if(method.getName().equals("addService") && args!=null && args.length>=2 && "activity".equals(args[0]) && args[1] instanceof IBinder && !(args[1] instanceof AppAttachment)) {
                localActivity.set((IBinder)args[1]);
                args=args.clone();args[1]=new AppAttachment((IBinder)args[1],publicPackage.get());
                System.err.println("POCKET_MICROG_APP_CACHE_READY");
            }
            try{return method.invoke(original,args);}
            catch(InvocationTargetException error){throw error.getCause();}
        });
        Field field=manager.getDeclaredField("sServiceManager");field.setAccessible(true);field.set(null,proxy);
        installed=true;
        System.err.println("POCKET_MICROG_PUBLICATION_READY uid=1000");
    }
    private static boolean candidate(String name) {
        return "com.google.android.gms".equals(name)||"com.android.vending".equals(name);
    }
    private static String hex(byte[] bytes) {
        char[] chars=new char[bytes.length*2];String alphabet="0123456789abcdef";
        for(int i=0;i<bytes.length;i++){int b=bytes[i]&255;chars[2*i]=alphabet.charAt(b>>4);chars[2*i+1]=alphabet.charAt(b&15);}return new String(chars);
    }
    private static byte[] unhex(String value) {
        if(value.length()%2!=0)throw new IllegalArgumentException("Certificate encoding");
        byte[] bytes=new byte[value.length()/2];
        for(int i=0;i<bytes.length;i++){int a=Character.digit(value.charAt(i*2),16),b=Character.digit(value.charAt(i*2+1),16);if(a<0||b<0)throw new IllegalArgumentException("Certificate encoding");bytes[i]=(byte)((a<<4)|b);}return bytes;
    }
    private static Signature googleCertificate()throws Exception {
        try(BufferedReader in=new BufferedReader(new InputStreamReader(new FileInputStream("/system/etc/pocket-microg/google-cert.hex"),"US-ASCII"))) {
            String line=in.readLine();if(line==null || line.length()>8192 || in.readLine()!=null)throw new IOException("Google certificate file");
            byte[] encoded=unhex(line);
            if(!hex(MessageDigest.getInstance("SHA-1").digest(encoded)).equals("38918a453d07199354f8b19af05ec6562ced5788"))throw new SecurityException("Google certificate digest");
            return new Signature(encoded);
        }
    }
    private static SigningInfo signingInfo(Signature signer)throws Exception {
        Class<?> details=Class.forName("android.content.pm.PackageParser$SigningDetails");
        Constructor<?> ctor=details.getConstructor(Signature[].class,int.class,Signature[].class,int[].class);
        Object value=ctor.newInstance(new Signature[]{signer},2,null,null);
        return (SigningInfo)SigningInfo.class.getConstructor(details).newInstance(value);
    }
    /** Keep AMS's standard app-binding cache consistent with service publication. */
    private static final class AppAttachment extends Binder {
        private final IBinder real;
        private final PackageQueries packages;
        private final int attach;
        private final Method commonServices;
        AppAttachment(IBinder binder,PackageQueries facade)throws Exception {
            if(facade==null)throw new IllegalStateException("Package service must precede activity service");
            real=binder;packages=facade;
            Class<?> api=Class.forName("android.app.IActivityManager$Stub");
            Field field=api.getDeclaredField("TRANSACTION_attachApplication");field.setAccessible(true);attach=field.getInt(null);
            // AMS lives in the system-server class loader, outside boot classes.
            commonServices=binder.getClass().getDeclaredMethod("getCommonServicesLocked",boolean.class);
            commonServices.setAccessible(true);attachInterface(null,"android.app.IActivityManager");
        }
        @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException {
            if(code==attach) {
                // Initialize at the normal first-bind lifecycle point, under
                // AMS's own lock. Preserve every other cached service. Forward
                // the actual attachment without changing its caller identity.
                synchronized(real) {
                    try {
                        for(boolean isolated:new boolean[]{false,true}) {
                            @SuppressWarnings("unchecked") Map<String,IBinder> cache=(Map<String,IBinder>)commonServices.invoke(real,isolated);
                            cache.put("package",packages);
                        }
                    }catch(ReflectiveOperationException error){throw new IllegalStateException("App package-service cache initialization failed",error);}
                }
            }
            return real.transact(code,data,reply,flags);
        }
    }
    private static final class PackageQueries extends Binder {
        private final IBinder real;
        private final Object packages;
        private final Class<?> api;
        private final Map<Integer,String> codes=new HashMap<>();
        PackageQueries(IBinder binder)throws Exception {
            real=binder;
            api=Class.forName(DESCRIPTOR);
            Class<?> stub=Class.forName(DESCRIPTOR+"$Stub");
            packages=stub.getMethod("asInterface",IBinder.class).invoke(null,real);
            for(String name:new String[]{"getPackageInfo","getPackageInfoVersioned","getInstalledPackages","getPackagesHoldingPermissions","hasSigningCertificate","hasUidSigningCertificate"}) {
                Field f=stub.getDeclaredField("TRANSACTION_"+name);f.setAccessible(true);codes.put(f.getInt(null),name);
            }
            attachInterface(null,DESCRIPTOR);
        }
        private Object call(String name,Class<?>[] types,Object... args)throws Exception {
            try{return api.getMethod(name,types).invoke(packages,args);}
            catch(InvocationTargetException error){Throwable cause=error.getCause();if(cause instanceof Exception)throw (Exception)cause;throw error;}
        }
        private boolean trusted(String name,int user)throws Exception {
            if(!candidate(name))return false;
            // This is the original local PackageManager binder, never our facade.
            PackageInfo info=(PackageInfo)call("getPackageInfo",new Class<?>[]{String.class,int.class,int.class},name,SIGNATURE_FLAGS,user);
            if(info==null || !name.equals(info.packageName))return false;
            Signature[] signers=info.signingInfo==null?info.signatures:info.signingInfo.getApkContentsSigners();
            return signers!=null && signers.length==1 && MICROG_CERT.equals(hex(MessageDigest.getInstance("SHA-256").digest(signers[0].toByteArray())));
        }
        private PackageInfo project(PackageInfo info,int flags,int user)throws Exception {
            if(info==null || (flags&SIGNATURE_FLAGS)==0 || !trusted(info.packageName,user))return info;
            // Never change PackageParser's signing details or cached arrays.
            Parcel copy=Parcel.obtain();PackageInfo result;
            try{info.writeToParcel(copy,0);copy.setDataPosition(0);result=PackageInfo.CREATOR.createFromParcel(copy);}finally{copy.recycle();}
            Signature google=googleCertificate();
            if((flags&PackageManager.GET_SIGNATURES)!=0)result.signatures=new Signature[]{google};
            if((flags&PackageManager.GET_SIGNING_CERTIFICATES)!=0)result.signingInfo=signingInfo(google);
            return result;
        }
        private boolean matchesGoogle(byte[] certificate,int type)throws Exception {
            if(certificate==null)return false;byte[] expected=googleCertificate().toByteArray();
            if(type==1)expected=MessageDigest.getInstance("SHA-256").digest(expected);else if(type!=0)return false;
            return MessageDigest.isEqual(certificate,expected);
        }
        private boolean trustedUid(int uid)throws Exception {
            String[] names=(String[])call("getPackagesForUid",new Class<?>[]{int.class},uid);
            if(names==null || names.length==0)return false;
            for(String name:names)if(!trusted(name,uid/100000))return false;
            return true;
        }
        @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException {
            String method=codes.get(code);
            // All installation, permission and signature-grant decisions use
            // the original implementation and original calling identity.
            if(method==null || reply==null)return real.transact(code,data,reply,flags);
            Parcel request=Parcel.obtain(),answer=Parcel.obtain();
            boolean delegated=false;
            try {
                request.appendFrom(data,0,data.dataSize());request.setDataPosition(0);request.enforceInterface(DESCRIPTOR);
                if(!real.transact(code,data,answer,flags))return false;
                delegated=true;
                answer.setDataPosition(0);
                try{answer.readException();}catch(RuntimeException original){reply.appendFrom(answer,0,answer.dataSize());return true;}
                if(method.equals("getPackageInfo")||method.equals("getPackageInfoVersioned")) {
                    if(method.equals("getPackageInfo"))request.readString();else {if(request.readInt()!=0)VersionedPackage.CREATOR.createFromParcel(request);}
                    int queryFlags=request.readInt(),user=request.readInt();
                    PackageInfo info=answer.readInt()==0?null:PackageInfo.CREATOR.createFromParcel(answer);
                    PackageInfo result=project(info,queryFlags,user);reply.writeNoException();reply.writeInt(result==null?0:1);if(result!=null)result.writeToParcel(reply,Parcelable.PARCELABLE_WRITE_RETURN_VALUE);
                } else if(method.equals("hasSigningCertificate")||method.equals("hasUidSigningCertificate")) {
                    String name=null;int uid=-1;
                    if(method.equals("hasSigningCertificate"))name=request.readString();else uid=request.readInt();
                    byte[] certificate=request.createByteArray();int type=request.readInt();boolean result=answer.readInt()!=0;
                    if(!result && matchesGoogle(certificate,type))result=name!=null?trusted(name,Binder.getCallingUid()/100000):trustedUid(uid);
                    reply.writeNoException();reply.writeInt(result?1:0);
                } else {
                    if(method.equals("getPackagesHoldingPermissions"))request.createStringArray();
                    int queryFlags=request.readInt(),user=request.readInt();int present=answer.readInt();
                    reply.writeNoException();reply.writeInt(present);
                    if(present!=0) {
                        Class<?> type=Class.forName("android.content.pm.ParceledListSlice");
                        Parcelable.Creator<?> creator=(Parcelable.Creator<?>)type.getField("CREATOR").get(null);
                        Parcelable slice=(Parcelable)creator.createFromParcel(answer);
                        @SuppressWarnings("unchecked") List<PackageInfo> list=(List<PackageInfo>)type.getMethod("getList").invoke(slice);
                        for(int i=0;i<list.size();i++)list.set(i,project(list.get(i),queryFlags,user));
                        slice.writeToParcel(reply,Parcelable.PARCELABLE_WRITE_RETURN_VALUE);
                    }
                }
                return true;
            } catch(Exception failure) {
                // Local Binder.transact can throw directly. Preserve the
                // original exception type, particularly permission denials.
                if(!delegated) {
                    if(failure instanceof RemoteException)throw (RemoteException)failure;
                    if(failure instanceof RuntimeException)throw (RuntimeException)failure;
                    throw new IllegalStateException(failure);
                }
                // A projection problem must not break unrelated package APIs.
                // Fall back to the original, unmodified package-service reply.
                System.err.println("POCKET_MICROG_QUERY_FALLBACK method="+method+" error="+failure);
                reply.setDataSize(0);reply.setDataPosition(0);reply.appendFrom(answer,0,answer.dataSize());return true;
            } finally {request.recycle();answer.recycle();}
        }
    }
}
