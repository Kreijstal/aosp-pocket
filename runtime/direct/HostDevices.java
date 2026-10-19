// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.guest;

import android.content.*;
import android.database.Cursor;
import android.net.*;
import android.os.*;
import android.provider.ContactsContract;
import dev.kreijstal.aosppocket.bridge.HostWire;
import org.json.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Guest-side consumers of the host broker; errors remain explicit. */
public final class HostDevices {
    private HostDevices() { }
    /** The control process is not an AMS app. Use the same external-provider
     * lifetime as Android 9's content command, under its guest system role. */
    private static final class Contacts implements AutoCloseable {
        private final Object manager;
        private final Class<?> api;
        private final IBinder token=new Binder();
        final ContentProviderClient client;
        Contacts(Context context) throws Exception {
            api=Class.forName("android.app.IActivityManager");
            manager=Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null);
            Object holder=api.getMethod("getContentProviderExternal",String.class,int.class,IBinder.class)
                .invoke(manager,ContactsContract.AUTHORITY,0,token);
            if(holder==null)throw new IOException("Guest ContactsProvider is unavailable");
            try {
                Class<?> provider=Class.forName("android.content.IContentProvider");
                Object value=holder.getClass().getField("provider").get(holder);
                client=ContentProviderClient.class.getConstructor(ContentResolver.class,provider,boolean.class)
                    .newInstance(context.getContentResolver(),value,true);
            } catch(Exception error){release();throw error;}
        }
        private void release() throws Exception {
            api.getMethod("removeContentProviderExternal",String.class,IBinder.class)
                .invoke(manager,ContactsContract.AUTHORITY,token);
        }
        public void close() throws Exception {try{client.close();}finally{release();}}
    }
    public static byte[] request(int operation,byte[] payload) throws IOException {
        try(LocalSocket socket=new LocalSocket()) {
            socket.connect(new LocalSocketAddress("/dev/socket/pocket_host",LocalSocketAddress.Namespace.FILESYSTEM));
            socket.setSoTimeout(15000);
            DataOutputStream out=new DataOutputStream(socket.getOutputStream());
            DataInputStream in=new DataInputStream(socket.getInputStream());
            out.writeInt(HostWire.MAGIC);out.writeInt(operation);HostWire.write(out,payload);out.flush();
            int status=in.readInt();byte[] result=HostWire.read(in,HostWire.MAX);
            if(status!=0)throw new IOException("Host bridge "+status+": "+new String(result,StandardCharsets.UTF_8));
            return result;
        }
    }
    public static JSONObject query(int operation,JSONObject args) throws Exception {
        return new JSONObject(new String(request(operation,args.toString().getBytes(StandardCharsets.UTF_8)),StandardCharsets.UTF_8));
    }
    interface Check {JSONObject run() throws Exception;}
    private static void check(JSONObject result,String name,Check action) throws Exception {
        try {result.put(name,action.run().put("status","ok"));}
        catch(Exception error){result.put(name,new JSONObject().put("status","unavailable").put("reason",error.toString()));}
    }
    static JSONObject diagnose(Context context) throws Exception {
        JSONObject result=new JSONObject().put("guest_elapsed_ms",SystemClock.elapsedRealtime());
        check(result,"host",()->query(HostWire.INFO,new JSONObject()));
        check(result,"contacts",()->{
            JSONObject host=query(HostWire.CONTACTS,new JSONObject());
            try(Contacts access=new Contacts(context)) {
            ContentProviderClient resolver=access.client;
            int guestCount=-1;
            try(Cursor c=resolver.query(ContactsContract.Contacts.CONTENT_URI,
                    new String[]{ContactsContract.Contacts._ID},null,null,null)) {if(c!=null)guestCount=c.getCount();}
            Map<Long,String> imported=new HashMap<>();
            try(Cursor c=resolver.query(ContactsContract.RawContacts.CONTENT_URI,
                    new String[]{ContactsContract.RawContacts._ID,ContactsContract.RawContacts.SYNC2},
                    ContactsContract.RawContacts.SYNC1+"=? AND "+ContactsContract.RawContacts.DELETED+"=0",
                    new String[]{"pocket-host-import-v1"},null)) {
                if(c!=null)while(c.moveToNext())imported.put(c.getLong(0),c.getString(1));
            }
            Set<String> values=new HashSet<>();
            try(Cursor c=resolver.query(ContactsContract.Data.CONTENT_URI,
                    new String[]{ContactsContract.Data.RAW_CONTACT_ID,ContactsContract.Data.MIMETYPE,
                        ContactsContract.Data.DATA1,ContactsContract.Data.DATA2},null,null,null)) {
                if(c!=null)while(c.moveToNext())if(imported.containsKey(c.getLong(0)))
                    values.add(imported.get(c.getLong(0))+"\n"+c.getString(1)+"\n"+c.getString(2)+"\n"+c.getInt(3));
            }
            JSONArray rows=host.getJSONArray("rows");int matched=0;
            for(int i=0;i<rows.length();i++) {
                JSONObject row=rows.getJSONObject(i);
                if(values.contains(row.getString("contact_id")+"\n"+row.getString("mimetype")+"\n"+
                    row.optString("value")+"\n"+row.optInt("type")))matched++;
            }
            return new JSONObject().put("host_contacts",host.getJSONArray("contacts").length()).put("host_rows",rows.length()).put("guest_contacts",guestCount)
                .put("guest_imported_contacts",imported.size()).put("matched_host_rows",matched);
            }
        });
        check(result,"location",()->{
            JSONObject location=query(HostWire.LOCATION,new JSONObject());
            return new JSONObject().put("provider",location.getString("provider")).put("age_ms",location.getLong("age_ms"));
        });
        check(result,"phone",()->query(HostWire.PHONE,new JSONObject()));
        check(result,"sensors",()->{
            JSONObject sensor=query(HostWire.SENSORS,new JSONObject().put("samples",4));
            return new JSONObject().put("samples",sensor.getJSONArray("events").length()).put("type",sensor.getInt("type"));
        });
        check(result,"camera",()->{
            JSONArray cameras=query(HostWire.CAMERAS,new JSONObject()).getJSONArray("devices");
            if(cameras.length()==0)throw new IOException("Host has no camera");
            JSONObject camera=cameras.getJSONObject(0);JSONArray sizes=camera.getJSONArray("yuv420_sizes");
            int width=0,height=0;long pixels=Long.MAX_VALUE;
            for(int i=0;i<sizes.length();i++) {
                JSONArray size=sizes.getJSONArray(i);long count=size.getLong(0)*size.getLong(1);
                if(count<pixels){width=size.getInt(0);height=size.getInt(1);pixels=count;}
            }
            byte[] packet=request(HostWire.CAMERA,new JSONObject().put("id",camera.getString("id"))
                .put("width",width).put("height",height).toString().getBytes(StandardCharsets.UTF_8));
            DataInputStream in=new DataInputStream(new ByteArrayInputStream(packet));
            JSONObject metadata=new JSONObject(new String(HostWire.read(in,65536),StandardCharsets.UTF_8));
            return new JSONObject().put("width",metadata.getInt("width")).put("height",metadata.getInt("height"))
                .put("format",metadata.getInt("format")).put("planes",metadata.getJSONArray("planes"))
                .put("bytes",in.available()).put("timestamp_ns",metadata.getLong("timestamp_ns"));
        });
        check(result,"audio",()->{
            byte[] packet=request(HostWire.MICROPHONE,new JSONObject().put("milliseconds",250)
                .put("sample_rate",48000).toString().getBytes(StandardCharsets.UTF_8));
            DataInputStream in=new DataInputStream(new ByteArrayInputStream(packet));
            JSONObject metadata=new JSONObject(new String(HostWire.read(in,8192),StandardCharsets.UTF_8));
            int size=in.available();if(size!=metadata.getInt("frames")*2)throw new IOException("Incomplete host PCM packet");
            int peak=0,nonzero=0;
            while(in.available()>=2) {int lo=in.readUnsignedByte(),hi=in.readUnsignedByte();int sample=(short)((hi<<8)|lo);
                if(sample!=0)nonzero++;peak=Math.max(peak,Math.abs(sample));}
            JSONObject playback=new JSONObject(new String(request(HostWire.PLAYBACK,packet),StandardCharsets.UTF_8));
            return new JSONObject().put("frames",metadata.getInt("frames")).put("sample_rate",48000)
                .put("nonzero_samples",nonzero).put("peak",peak).put("playback_frames",playback.getInt("frames"));
        });
        return result;
    }
    static String importContacts(Context context) throws Exception {
        JSONObject snapshot=query(HostWire.CONTACTS,new JSONObject());
        if(snapshot.getBoolean("truncated"))throw new IOException("Host contact snapshot exceeded the limit; nothing imported");
        JSONArray rows=snapshot.getJSONArray("rows");
        Map<String,List<JSONObject>> contacts=new LinkedHashMap<>();
        Map<String,String> names=new HashMap<>();JSONArray hostContacts=snapshot.getJSONArray("contacts");
        for(int i=0;i<hostContacts.length();i++) {
            JSONObject contact=hostContacts.getJSONObject(i);String id=contact.getString("contact_id");
            contacts.put(id,new ArrayList<>());names.put(id,contact.optString("name"));
        }
        for(int i=0;i<rows.length();i++) {
            JSONObject row=rows.getJSONObject(i);String id=row.getString("contact_id");
            List<JSONObject> data=contacts.get(id);if(data==null){data=new ArrayList<>();contacts.put(id,data);}data.add(row);
        }
        try(Contacts access=new Contacts(context)) {
        ContentProviderClient resolver=access.client;Map<String,Long> existing=new HashMap<>();
        try(Cursor c=resolver.query(ContactsContract.RawContacts.CONTENT_URI,
                new String[]{ContactsContract.RawContacts._ID,ContactsContract.RawContacts.SYNC2},
                ContactsContract.RawContacts.SYNC1+"=? AND "+ContactsContract.RawContacts.DELETED+"=0",
                new String[]{"pocket-host-import-v1"},null)) {
            if(c==null)throw new IOException("Guest ContactsProvider is unavailable");
            while(c.moveToNext())existing.put(c.getString(1),c.getLong(0));
        }
        int imported=0;
        for(Map.Entry<String,List<JSONObject>> entry:contacts.entrySet()) {
            ArrayList<ContentProviderOperation> operations=new ArrayList<>();Long raw=existing.get(entry.getKey());
            if(raw==null)operations.add(ContentProviderOperation.newInsert(ContactsContract.RawContacts.CONTENT_URI)
                .withValue(ContactsContract.RawContacts.ACCOUNT_NAME,null).withValue(ContactsContract.RawContacts.ACCOUNT_TYPE,null)
                .withValue(ContactsContract.RawContacts.SYNC1,"pocket-host-import-v1")
                .withValue(ContactsContract.RawContacts.SYNC2,entry.getKey()).build());
            else operations.add(ContentProviderOperation.newDelete(ContactsContract.Data.CONTENT_URI)
                .withSelection(ContactsContract.Data.RAW_CONTACT_ID+"=?",new String[]{Long.toString(raw)}).build());
            ContentProviderOperation.Builder name=ContentProviderOperation.newInsert(ContactsContract.Data.CONTENT_URI);
            if(raw==null)name.withValueBackReference(ContactsContract.Data.RAW_CONTACT_ID,0);
            else name.withValue(ContactsContract.Data.RAW_CONTACT_ID,raw);
            operations.add(name.withValue(ContactsContract.Data.MIMETYPE,ContactsContract.CommonDataKinds.StructuredName.CONTENT_ITEM_TYPE)
                .withValue(ContactsContract.CommonDataKinds.StructuredName.DISPLAY_NAME,names.get(entry.getKey())).build());
            for(JSONObject row:entry.getValue()) {
                ContentProviderOperation.Builder data=ContentProviderOperation.newInsert(ContactsContract.Data.CONTENT_URI);
                if(raw==null)data.withValueBackReference(ContactsContract.Data.RAW_CONTACT_ID,0);
                else data.withValue(ContactsContract.Data.RAW_CONTACT_ID,raw);
                operations.add(data.withValue(ContactsContract.Data.MIMETYPE,row.getString("mimetype"))
                    .withValue(ContactsContract.Data.DATA1,row.optString("value"))
                    .withValue(ContactsContract.Data.DATA2,row.optInt("type")).build());
            }
            resolver.applyBatch(operations);imported++;
        }
        for(Map.Entry<String,Long> old:existing.entrySet())if(!contacts.containsKey(old.getKey()))
            resolver.delete(ContentUris.withAppendedId(ContactsContract.RawContacts.CONTENT_URI,old.getValue()),null,null);
        // Existing unrelated contacts and host records are never modified.
        return "Imported "+imported+" host contacts into the guest";
        }
    }
}
