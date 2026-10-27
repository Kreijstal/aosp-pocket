// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketmicrogtest;

import android.app.Activity;
import android.accounts.*;
import android.content.*;
import android.content.pm.*;
import android.os.*;
import android.widget.TextView;
import java.io.*;
import java.lang.reflect.*;
import java.security.MessageDigest;
import java.util.concurrent.*;
import org.json.JSONObject;

/** Real app client: certificate queries, caller isolation and offline service binding. */
public final class MainActivity extends Activity {
    private final JSONObject report=new JSONObject();
    private static final String GOOGLE="38918a453d07199354f8b19af05ec6562ced5788";
    private void check(String key,boolean value)throws Exception {report.put(key,value);if(!value)throw new AssertionError(key);}
    private static String digest(Signature signature)throws Exception {
        byte[] d=MessageDigest.getInstance("SHA-1").digest(signature.toByteArray());StringBuilder s=new StringBuilder();
        for(byte b:d)s.append(String.format("%02x",b&255));return s.toString();
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);TextView view=new TextView(this);view.setText("Testing microG API compatibility");setContentView(view);
        new Thread(()->{
            try{test();report.put("validation","PASS");}
            catch(Throwable error){try{report.put("validation","FAIL");report.put("error",String.valueOf(error));}catch(Exception ignored){}}
            try(FileOutputStream out=openFileOutput("report.json",0)){out.write(report.toString(2).getBytes("UTF-8"));}catch(Exception error){android.util.Log.e("PocketMicrogTest","report",error);}
            runOnUiThread(()->view.setText(report.toString()));
        },"MicrogApiTest").start();
    }
    private void test()throws Exception {
        PackageManager pm=getPackageManager();report.put("uid",android.os.Process.myUid());report.put("sdk",Build.VERSION.SDK_INT);
        check("ordinary_app",android.os.Process.myUid()>=10000);
        byte[] google=null;
        for(String name:new String[]{"com.google.android.gms","com.android.vending"}) {
            PackageInfo legacy=pm.getPackageInfo(name,PackageManager.GET_SIGNATURES);
            report.put(name+".legacy_digest",legacy.signatures==null||legacy.signatures.length==0?"absent":digest(legacy.signatures[0]));
            check(name+".legacy",legacy.signatures.length==1 && GOOGLE.equals(digest(legacy.signatures[0])));
            google=legacy.signatures[0].toByteArray();
            PackageInfo modern=pm.getPackageInfo(name,PackageManager.GET_SIGNING_CERTIFICATES);
            check(name+".modern",GOOGLE.equals(digest(modern.signingInfo.getApkContentsSigners()[0])));
            check(name+".history",!modern.signingInfo.hasMultipleSigners() && GOOGLE.equals(digest(modern.signingInfo.getSigningCertificateHistory()[0])));
            check(name+".versioned",GOOGLE.equals(digest(pm.getPackageInfo(new VersionedPackage(name,legacy.versionCode),PackageManager.GET_SIGNING_CERTIFICATES).signingInfo.getApkContentsSigners()[0])));
            check(name+".raw_cert",pm.hasSigningCertificate(name,google,PackageManager.CERT_INPUT_RAW_X509));
            byte[] sha=MessageDigest.getInstance("SHA-256").digest(google);
            check(name+".sha256_cert",pm.hasSigningCertificate(name,sha,PackageManager.CERT_INPUT_SHA256));
            check(name+".uid_cert",pm.hasSigningCertificate(legacy.applicationInfo.uid,sha,PackageManager.CERT_INPUT_SHA256));
            sha[0]^=1;check(name+".wrong_cert_rejected",!pm.hasSigningCertificate(name,sha,PackageManager.CERT_INPUT_SHA256));
            boolean found=false;for(PackageInfo info:pm.getInstalledPackages(PackageManager.GET_SIGNATURES|PackageManager.GET_SIGNING_CERTIFICATES))if(name.equals(info.packageName))found=GOOGLE.equals(digest(info.signatures[0]))&&GOOGLE.equals(digest(info.signingInfo.getApkContentsSigners()[0]));
            check(name+".list",found);
            found=false;for(PackageInfo info:pm.getPackagesHoldingPermissions(new String[]{"android.permission.INTERNET"},PackageManager.GET_SIGNING_CERTIFICATES))if(name.equals(info.packageName))found=GOOGLE.equals(digest(info.signingInfo.getApkContentsSigners()[0]));
            check(name+".permission_list",found==(pm.checkPermission("android.permission.INTERNET",name)==PackageManager.PERMISSION_GRANTED));
        }
        check("self_not_projected",!GOOGLE.equals(digest(pm.getPackageInfo(getPackageName(),PackageManager.GET_SIGNATURES).signatures[0])));
        check("self_certificate_rejected",!pm.hasSigningCertificate(getPackageName(),google,PackageManager.CERT_INPUT_RAW_X509));
        check("self_uid_certificate_rejected",!pm.hasSigningCertificate(android.os.Process.myUid(),google,PackageManager.CERT_INPUT_RAW_X509));
        check("system_signature_not_granted",pm.checkSignatures("com.google.android.gms","android")==PackageManager.SIGNATURE_NO_MATCH);
        Object binder=Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,"package");
        Object api=Class.forName("android.content.pm.IPackageManager$Stub").getMethod("asInterface",IBinder.class).invoke(null,binder);
        Method managerMethod=Class.forName("android.os.ServiceManager").getDeclaredMethod("getIServiceManager");managerMethod.setAccessible(true);
        Object manager=managerMethod.invoke(null);
        IBinder direct=(IBinder)Class.forName("android.os.IServiceManager").getMethod("getService",String.class).invoke(manager,"package");
        Object directApi=Class.forName("android.content.pm.IPackageManager$Stub").getMethod("asInterface",IBinder.class).invoke(null,direct);
        PackageInfo directInfo=(PackageInfo)Class.forName("android.content.pm.IPackageManager").getMethod("getPackageInfo",String.class,int.class,int.class).invoke(directApi,"com.google.android.gms",PackageManager.GET_SIGNING_CERTIFICATES,0);
        check("direct_service_lookup",GOOGLE.equals(digest(directInfo.signingInfo.getApkContentsSigners()[0])));
        int anyUser=PackageManager.class.getField("MATCH_ANY_USER").getInt(null);
        boolean crossUserDenied=false;
        try{Class.forName("android.content.pm.IPackageManager").getMethod("getPackageInfo",String.class,int.class,int.class).invoke(api,"com.google.android.gms",anyUser|PackageManager.GET_SIGNATURES,0);}
        catch(InvocationTargetException e){crossUserDenied=e.getCause() instanceof SecurityException;}
        check("cross_user_flag_denied",crossUserDenied);
        boolean listDenied=false;
        try{pm.getInstalledPackages(anyUser|PackageManager.GET_SIGNING_CERTIFICATES);}
        catch(SecurityException e){listDenied=true;}
        check("cross_user_list_denied",listDenied);
        boolean grantDenied=false;
        try{Class.forName("android.content.pm.IPackageManager").getMethod("grantRuntimePermission",String.class,String.class,int.class).invoke(api,getPackageName(),"android.permission.READ_CONTACTS",0);}
        catch(InvocationTargetException e){grantDenied=e.getCause() instanceof SecurityException;}
        check("permission_grant_denied",grantDenied);
        check("permission_remains_denied",checkSelfPermission("android.permission.READ_CONTACTS")==PackageManager.PERMISSION_DENIED);
        boolean bootstrapDenied=false;
        try{Class.forName("dev.kreijstal.aosppocket.guest.MicrogSupport").getMethod("install").invoke(null);}
        catch(InvocationTargetException e){bootstrapDenied=e.getCause() instanceof SecurityException;}
        catch(ReflectiveOperationException|SecurityException e){bootstrapDenied=true;}
        check("bootstrap_not_callable_by_app",bootstrapDenied);
        boolean auth=false;for(AuthenticatorDescription a:AccountManager.get(this).getAuthenticatorTypes())if("com.google".equals(a.type))auth=true;
        check("google_account_authenticator",auth);
        CountDownLatch bound=new CountDownLatch(1);final IBinder[] service={null};
        ServiceConnection connection=new ServiceConnection(){
            public void onServiceConnected(ComponentName name,IBinder binder){service[0]=binder;bound.countDown();}
            public void onServiceDisconnected(ComponentName name){}
            public void onNullBinding(ComponentName name){bound.countDown();}
        };
        check("gms_service_resolved",bindService(new Intent("com.google.android.gms.common.service.START").setPackage("com.google.android.gms"),connection,Context.BIND_AUTO_CREATE));
        try {
            check("gms_service_connected",bound.await(60,TimeUnit.SECONDS) && service[0]!=null);
            check("gms_service_broker", "com.google.android.gms.common.internal.IGmsServiceBroker".equals(service[0].getInterfaceDescriptor()));
            check("gms_binder_alive",service[0].pingBinder());
            requestCommonService(service[0]);
        }finally{unbindService(connection);}
    }
    private void requestCommonService(IBinder broker)throws Exception {
        CountDownLatch done=new CountDownLatch(1);final int[] status={-1};final IBinder[] common={null};
        Binder callback=new Binder(){
            @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException {
                if(code!=1 && code!=3)return super.onTransact(code,data,reply,flags);
                data.enforceInterface("com.google.android.gms.common.internal.IGmsCallbacks");
                status[0]=data.readInt();common[0]=data.readStrongBinder();done.countDown();
                if(reply!=null)reply.writeNoException();return true;
            }
        };
        Parcel request=Parcel.obtain(),reply=Parcel.obtain();
        try {
            request.writeInterfaceToken("com.google.android.gms.common.internal.IGmsServiceBroker");request.writeStrongBinder(callback);request.writeInt(1);
            // Android SafeParcelable object header and GetServiceRequest fields.
            request.writeInt(0xffff4f45);int length=request.dataPosition();request.writeInt(0);int start=request.dataPosition();
            request.writeInt(0x40001);request.writeInt(6);
            request.writeInt(0x40002);request.writeInt(39);
            request.writeInt(0x40003);request.writeInt(252432000);
            request.writeInt(0xffff0004);int stringLength=request.dataPosition();request.writeInt(0);int stringStart=request.dataPosition();
            request.writeString(getPackageName());int end=request.dataPosition();request.setDataPosition(stringLength);request.writeInt(end-stringStart);request.setDataPosition(end);
            request.writeInt(0x4000c);request.writeInt(0);end=request.dataPosition();request.setDataPosition(length);request.writeInt(end-start);request.setDataPosition(end);
            check("gms_get_service_transacted",broker.transact(46,request,reply,0));reply.readException();
            check("gms_get_service_success",done.await(30,TimeUnit.SECONDS) && status[0]==0 && common[0]!=null);
            check("gms_common_service", "com.google.android.gms.common.internal.service.ICommonService".equals(common[0].getInterfaceDescriptor()));
            CountDownLatch called=new CountDownLatch(1);final int[] commonStatus={-1};
            Binder commonCallback=new Binder(){
                @Override protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException {
                    if(code!=1)return super.onTransact(code,data,reply,flags);
                    data.enforceInterface("com.google.android.gms.common.internal.service.ICommonCallbacks");
                    commonStatus[0]=data.readInt();called.countDown();if(reply!=null)reply.writeNoException();return true;
                }
            };
            Parcel call=Parcel.obtain(),response=Parcel.obtain();
            try {
                // This probe has no account or selected default. Exercise its
                // own empty client state through the actual microG API.
                call.writeInterfaceToken("com.google.android.gms.common.internal.service.ICommonService");call.writeStrongBinder(commonCallback);
                check("gms_common_rpc_transacted",common[0].transact(1,call,response,0));response.readException();
                check("gms_common_rpc_success",called.await(30,TimeUnit.SECONDS) && commonStatus[0]==0);
            }finally{call.recycle();response.recycle();}
        }finally{request.recycle();reply.recycle();}
    }
}
