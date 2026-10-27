// SPDX-License-Identifier: GPL-3.0-only
import android.content.ContentValues;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Looper;
import android.provider.ContactsContract;
import android.provider.Settings;
import java.io.*;
import java.security.MessageDigest;
import java.util.UUID;
import org.json.*;

/** Owner-console signing/data checks; seed only in an isolated disposable guest. */
public final class GuestSigningProbe {
    private static final String CANARY="pocket_resign_fixture_canary";
    /** Owner consoles use AMS's external-provider API, like Android's content CLI. */
    private static final class Providers implements AutoCloseable {
        final Object am;final Class<?> api;final android.os.IBinder token=new android.os.Binder();
        final java.util.List<String> acquired=new java.util.ArrayList<>();
        Providers(Context context,Object activityThread)throws Exception {
            Class<?> manager=Class.forName("android.app.ActivityManager");
            java.lang.reflect.Method service=manager.getDeclaredMethod("getService");service.setAccessible(true);am=service.invoke(null);
            api=Class.forName("android.app.IActivityManager");Class<?> holder=Class.forName("android.app.ContentProviderHolder");
            java.lang.reflect.Method install=activityThread.getClass().getDeclaredMethod("installProvider",Context.class,holder,android.content.pm.ProviderInfo.class,boolean.class,boolean.class,boolean.class);
            install.setAccessible(true);
            try {
                for(String authority:new String[]{"settings","com.android.contacts"}) {
                    Object value=api.getMethod("getContentProviderExternal",String.class,int.class,android.os.IBinder.class).invoke(am,authority,0,token);
                    if(value==null)throw new IOException("No external provider: "+authority);
                    acquired.add(authority);
                    install.invoke(activityThread,context,value,holder.getField("info").get(value),false,true,true);
                }
            }catch(Exception error){close();throw error;}
        }
        public void close()throws Exception {
            for(String authority:acquired)api.getMethod("removeContentProviderExternal",String.class,android.os.IBinder.class).invoke(am,authority,token);
            acquired.clear();
        }
    }
    private static String digest(byte[] value)throws Exception {
        StringBuilder out=new StringBuilder();for(byte b:MessageDigest.getInstance("SHA-256").digest(value))out.append(String.format("%02x",b&255));return out.toString();
    }
    private static JSONObject read(String path)throws Exception {
        ByteArrayOutputStream data=new ByteArrayOutputStream();byte[] buffer=new byte[8192];
        try(InputStream in=new FileInputStream(path)){int n;while((n=in.read(buffer))!=-1){if(data.size()+n>1024*1024)throw new IOException("Oversized signing fixture");data.write(buffer,0,n);}}
        return new JSONObject(new String(data.toByteArray(),"UTF-8"));
    }
    private static void save(String path,JSONObject data)throws Exception {
        try(FileOutputStream out=new FileOutputStream(path)){out.write(data.toString(2).getBytes("UTF-8"));out.getFD().sync();}
    }
    private static JSONObject packages(Context context)throws Exception {
        JSONObject result=new JSONObject();
        for(PackageInfo p:context.getPackageManager().getInstalledPackages(PackageManager.GET_SIGNATURES|PackageManager.GET_SIGNING_CERTIFICATES)) {
            if(p.applicationInfo==null)continue;
            android.content.pm.Signature[] certs=p.signingInfo==null?p.signatures:p.signingInfo.getApkContentsSigners();
            JSONObject row=new JSONObject();row.put("uid",p.applicationInfo.uid);row.put("path",p.applicationInfo.sourceDir);
            if(certs!=null && certs.length==1)row.put("signerSha256",digest(certs[0].toByteArray()));
            result.put(p.packageName,row);
        }
        return result;
    }
    private static int verifyCertificates(JSONObject actual,JSONObject expected)throws Exception {
        int checked=0;JSONObject paths=expected.getJSONObject("apks"),certs=expected.getJSONObject("certificates");
        for(java.util.Iterator<String> it=actual.keys();it.hasNext();) {
            String name=it.next();JSONObject row=actual.getJSONObject(name);String path=row.getString("path");
            int system=path.indexOf("/system/");if(system<0)continue;
            path=path.substring(system+1);String role=paths.optString(path,"");if(role.isEmpty())continue;
            if(!row.getString("signerSha256").equals(certs.getString(role)))throw new SecurityException("Wrong installed signer: "+name);
            checked++;
        }
        if(checked<20)throw new SecurityException("Too few installed system certificates checked: "+checked);
        return checked;
    }
    public static void main(String[] args) {
        try {
            android.system.Os.setgid(1000);android.system.Os.setuid(1000);
            Looper.prepareMainLooper();Class<?> thread=Class.forName("android.app.ActivityThread");
            Object main=thread.getMethod("systemMain").invoke(null);
            Context context=(Context)thread.getMethod("getSystemContext").invoke(main);
            JSONObject actual=packages(context),report=new JSONObject();
            try(Providers providers=args[0].equals("inspect")?null:new Providers(context,main)) {
            if(args[0].equals("seed")) {
                String token=UUID.randomUUID().toString();
                if(!Settings.Global.putString(context.getContentResolver(),CANARY,token))throw new IOException("Settings seed failed");
                ContentValues raw=new ContentValues();raw.putNull(ContactsContract.RawContacts.ACCOUNT_NAME);raw.putNull(ContactsContract.RawContacts.ACCOUNT_TYPE);
                Uri contact=context.getContentResolver().insert(ContactsContract.RawContacts.CONTENT_URI,raw);
                if(contact==null)throw new IOException("Contacts seed failed");
                ContentValues value=new ContentValues();value.put(ContactsContract.Data.RAW_CONTACT_ID,Long.parseLong(contact.getLastPathSegment()));
                value.put(ContactsContract.Data.MIMETYPE,ContactsContract.CommonDataKinds.StructuredName.CONTENT_ITEM_TYPE);
                value.put(ContactsContract.CommonDataKinds.StructuredName.DISPLAY_NAME,"Pocket Signing Fixture "+token);
                Uri row=context.getContentResolver().insert(ContactsContract.Data.CONTENT_URI,value);
                if(row==null)throw new IOException("Contacts data seed failed");
                report.put("settingsCanary",token);report.put("contactDataUri",row.toString());report.put("packages",actual);
                save(args[1],report);System.out.println("SIGNING_FIXTURE_SEEDED");
            } else {
                JSONObject expected=read("/system/etc/pocket-release-signing.json");
                report.put("installedCertificatesChecked",verifyCertificates(actual,expected));
                if(args[0].equals("verify")) {
                    JSONObject before=read(args[1]);String token=before.getString("settingsCanary");
                    if(!token.equals(Settings.Global.getString(context.getContentResolver(),CANARY)))throw new SecurityException("Settings data changed");
                    try(Cursor cursor=context.getContentResolver().query(Uri.parse(before.getString("contactDataUri")),new String[]{ContactsContract.Data.DATA1},null,null,null)) {
                        if(cursor==null || !cursor.moveToFirst() || !("Pocket Signing Fixture "+token).equals(cursor.getString(0)))throw new SecurityException("Contacts data changed");
                    }
                    JSONObject old=before.getJSONObject("packages");
                    for(String name:new String[]{"android","com.android.settings","com.android.providers.settings","com.android.providers.contacts"})
                        if(actual.getJSONObject(name).getInt("uid")!=old.getJSONObject(name).getInt("uid"))throw new SecurityException("System UID changed: "+name);
                    report.put("settingsDataPreserved",true);report.put("contactsDataPreserved",true);report.put("systemUidsPreserved",true);
                }
                report.put("validation","PASS");report.put("packages",actual);save(args[args.length-1],report);
                System.out.println("GUEST_SIGNING_RESULT "+report);
            }
            }
            System.exit(0);
        }catch(Throwable error){error.printStackTrace();System.exit(1);}
    }
}
