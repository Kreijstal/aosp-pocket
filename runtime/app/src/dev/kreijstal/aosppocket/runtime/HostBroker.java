// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.Manifest;
import android.content.*;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.hardware.*;
import android.hardware.camera2.*;
import android.location.*;
import android.net.*;
import android.os.*;
import android.provider.ContactsContract;
import android.telephony.TelephonyManager;
import dev.kreijstal.aosppocket.bridge.HostWire;
import org.json.*;
import java.io.*;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;

/** Host Android APIs only. A guest never opens host HALs or a host modem. */
final class HostBroker implements AutoCloseable {
    private final Context context;
    private final File endpoint;
    private final File root;
    private final boolean api32Dns;
    private final LocalSocket bound=new LocalSocket();
    private final LocalServerSocket server;
    private final ThreadPoolExecutor workers=new ThreadPoolExecutor(4,4,0,TimeUnit.SECONDS,new ArrayBlockingQueue<>(8));
    private final Set<LocalSocket> clients=ConcurrentHashMap.newKeySet();
    private volatile boolean closed;
    HostBroker(Context owner,File root) throws IOException {
        context=owner.getApplicationContext();this.root=root;api32Dns=HostDns.api32(root);endpoint=new File(root,"dev/socket/pocket_host");
        if(endpoint.exists() && !endpoint.delete())throw new IOException("Cannot reset host broker endpoint");
        bound.bind(new LocalSocketAddress(endpoint.getPath(),LocalSocketAddress.Namespace.FILESYSTEM));
        server=new LocalServerSocket(bound.getFileDescriptor());
        new Thread(this::accept,"PocketHostBroker").start();
    }
    static boolean permitted(Context c,String permission) {return c.checkSelfPermission(permission)==PackageManager.PERMISSION_GRANTED;}
    private void require(String permission) {
        if(!permitted(context,permission))throw new SecurityException("Host permission required: "+permission);
    }
    static JSONObject info(Context c) throws Exception {
        JSONObject result=new JSONObject().put("protocol",1).put("host_sdk",Build.VERSION.SDK_INT);
        result.put("network",network(c));
        JSONObject permissions=new JSONObject();
        for(String p:new String[]{Manifest.permission.CAMERA,Manifest.permission.RECORD_AUDIO,
            Manifest.permission.READ_CONTACTS,Manifest.permission.READ_PHONE_STATE,
            Manifest.permission.ACCESS_FINE_LOCATION,Manifest.permission.ACCESS_COARSE_LOCATION})
            permissions.put(p,permitted(c,p));
        result.put("permissions",permissions);
        result.put("camera_count",((CameraManager)c.getSystemService(Context.CAMERA_SERVICE)).getCameraIdList().length);
        result.put("sensor_count",((SensorManager)c.getSystemService(Context.SENSOR_SERVICE)).getSensorList(Sensor.TYPE_ALL).size());
        result.put("ims",new JSONObject().put("owner","host").put("guest_registration","unavailable")
            .put("calls","host dialer delegation").put("sms","host composer delegation")
            .put("reason","Carrier/privileged IMS APIs are not available to an ordinary host app"));
        result.put("standard_guest_apis",new JSONObject().put("battery","relayed")
            .put("dns","host resolver via guest dnsproxyd").put("camera","HAL adapter pending")
            .put("audio","output via guest audio HAL; input pending").put("sensors","HAL adapter pending")
            .put("contacts","explicit import").put("phone","radio adapter pending"));
        return result;
    }
    static JSONObject network(Context c) throws Exception {
        return HostDns.network(c);
    }
    private void accept() {
        while(!closed) {
            LocalSocket client=null;
            try {
                client=server.accept();
                if(client.getPeerCredentials().getUid()!=android.os.Process.myUid()){client.close();continue;}
                clients.add(client);final LocalSocket accepted=client;workers.execute(()->serve(accepted));
            } catch(Exception error) {
                if(client!=null){clients.remove(client);try{client.close();}catch(IOException ignored){}}
                if(closed)break;
                android.util.Log.w("PocketHostBroker","Host bridge client unavailable: "+error.getClass().getSimpleName());
            }
        }
    }
    private void serve(LocalSocket client) {
        try(LocalSocket peer=client) {
            peer.setSoTimeout(15000);
            DataInputStream in=new DataInputStream(peer.getInputStream());
            DataOutputStream out=new DataOutputStream(peer.getOutputStream());
            if(in.readInt()!=HostWire.MAGIC)throw new IOException("Unsupported host bridge protocol");
            int op=in.readInt();byte[] request=HostWire.read(in,op==HostWire.PLAYBACK?HostWire.MAX:16384);
            if(op==HostWire.AUDIO_OUT && !closed) {
                try {HostMedia.stream(peer,new JSONObject(new String(request,StandardCharsets.UTF_8)),in,out);return;}
                catch(IOException ended){if(ended instanceof java.net.SocketTimeoutException || closed)return;throw ended;}
                catch(Exception error){out.writeInt(503);HostWire.write(out,("Guest audio output unavailable: "+error.getClass().getSimpleName()).getBytes(StandardCharsets.UTF_8));out.flush();return;}
            }
            if(op==HostWire.CAMERA_STREAM && !closed) {
                try {require(Manifest.permission.CAMERA);HostMedia.cameraStream(context,new JSONObject(new String(request,StandardCharsets.UTF_8)),out);return;}
                catch(SecurityException denied){out.writeInt(403);HostWire.write(out,denied.getMessage().getBytes(StandardCharsets.UTF_8));out.flush();return;}
                catch(IllegalArgumentException invalid){out.writeInt(400);HostWire.write(out,"Invalid camera stream request".getBytes(StandardCharsets.UTF_8));out.flush();return;}
                catch(IOException ended){if(ended instanceof java.net.SocketTimeoutException || closed)return;throw ended;}
                catch(Exception error){out.writeInt(503);HostWire.write(out,("Guest camera stream unavailable: "+error.getClass().getSimpleName()).getBytes(StandardCharsets.UTF_8));out.flush();return;}
            }
            if(op==HostWire.AUDIO_IN && !closed) {
                try {require(Manifest.permission.RECORD_AUDIO);HostMedia.capture(peer,new JSONObject(new String(request,StandardCharsets.UTF_8)),out);return;}
                catch(SecurityException denied){out.writeInt(403);HostWire.write(out,denied.getMessage().getBytes(StandardCharsets.UTF_8));out.flush();return;}
                catch(IOException ended){if(ended instanceof java.net.SocketTimeoutException || closed)return;throw ended;}
                catch(Exception error){out.writeInt(503);HostWire.write(out,("Guest audio input unavailable: "+error.getClass().getSimpleName()).getBytes(StandardCharsets.UTF_8));out.flush();return;}
            }
            int status=0;byte[] response;
            if(api32Dns && (op==HostWire.DNS || op==HostWire.DNS_SERVERS || op==HostWire.DNS_PACKET) && !HostDns.trustedPeer(root,peer.getPeerCredentials().getPid())) {
                out.writeInt(403);HostWire.write(out,new byte[0]);out.flush();return;
            }
            try {response=handle(op,request);}
            catch(SecurityException denied){status=403;response=denied.getMessage().getBytes(StandardCharsets.UTF_8);}
            catch(IllegalArgumentException invalid){status=400;response="Invalid host bridge request".getBytes(StandardCharsets.UTF_8);}
            catch(UnsupportedOperationException unavailable){status=404;response=unavailable.getMessage().getBytes(StandardCharsets.UTF_8);}
            catch(java.net.SocketTimeoutException | java.util.concurrent.TimeoutException timeout){status=504;response="Host operation timed out".getBytes(StandardCharsets.UTF_8);}
            catch(Exception error){status=503;response=("Host operation unavailable: "+error.getClass().getSimpleName()).getBytes(StandardCharsets.UTF_8);}
            out.writeInt(status);HostWire.write(out,response);out.flush();
        } catch(IOException ignored) { /* A stopped guest or cancelled operation may close its socket. */ }
        finally {clients.remove(client);}
    }
    private static byte[] json(JSONObject value) {return value.toString().getBytes(StandardCharsets.UTF_8);}
    private byte[] handle(int op,byte[] request) throws Exception {
        if(closed)throw new IOException("Guest session ended");
        if(op==HostWire.INFO)return json(info(context));
        if(op==HostWire.DNS)return HostDns.resolve(request);
        if(op==HostWire.DNS_PACKET) {
            if(!api32Dns)throw new UnsupportedOperationException("Raw DNS requires the API32 resolver service");
            return HostDnsPacket.resolve(context,request);
        }
        if(op==HostWire.DNS_SERVERS) {
            if(request.length!=0)throw new IllegalArgumentException("DNS server query has no payload");
            return HostDns.servers(context);
        }
        if(op==HostWire.PLAYBACK)return json(HostMedia.play(context,request));
        JSONObject args=request.length==0?new JSONObject():new JSONObject(new String(request,StandardCharsets.UTF_8));
        switch(op) {
        case HostWire.CONTACTS:require(Manifest.permission.READ_CONTACTS);return json(contacts());
        case HostWire.LOCATION:return json(location());
        case HostWire.PHONE:require(Manifest.permission.READ_PHONE_STATE);return json(phone());
        case HostWire.PHONE_UI:return json(phoneUi(args));
        case HostWire.SENSORS:return json(sensors(args));
        // Camera2 exposes public IDs and stream capabilities before CAMERA is
        // granted. Enumerate at boot; opening either capture path still checks
        // the host permission above/below, including after permission changes.
        case HostWire.CAMERAS:
            return "lines".equals(args.optString("format"))?HostMedia.cameraLines(context):json(HostMedia.cameras(context));
        case HostWire.CAMERA:require(Manifest.permission.CAMERA);return HostMedia.camera(context,args);
        case HostWire.MICROPHONE:require(Manifest.permission.RECORD_AUDIO);return HostMedia.record(context,args);
        case HostWire.VIBRATE:
            // The guest Vibrator HAL forwards on()/off()/setAmplitude() and predefined effects here. Zero cancels.
            Vibrator vibrator=(Vibrator)context.getSystemService(Context.VIBRATOR_SERVICE);
            if(!vibrator.hasVibrator())throw new UnsupportedOperationException("Host has no vibrator");
            JSONObject accepted=new JSONObject().put("accepted",true).put("amplitude_control",vibrator.hasAmplitudeControl());
            if(args.has("effect"))return json(accepted.put("length_ms",effect(vibrator,args.getInt("effect"),args.optInt("strength",1))));
            long duration=args.optLong("milliseconds",30);int amplitude=args.optInt("amplitude",VibrationEffect.DEFAULT_AMPLITUDE);
            if(duration<0 || duration>30000)throw new IllegalArgumentException("Invalid vibration duration");
            if(amplitude!=VibrationEffect.DEFAULT_AMPLITUDE && (amplitude<1 || amplitude>255))throw new IllegalArgumentException("Invalid vibration amplitude");
            if(duration==0)vibrator.cancel();
            else vibrator.vibrate(VibrationEffect.createOneShot(duration,vibrator.hasAmplitudeControl()?amplitude:VibrationEffect.DEFAULT_AMPLITUDE));
            return json(accepted);
        default:throw new UnsupportedOperationException("Host operation is not implemented");
        }
    }
    // Guest Vibrator 1.2 effect ids (CLICK, DOUBLE_CLICK, TICK, THUD, POP, HEAVY_CLICK) equal the host's VibrationEffect ids.
    // Host primitives carry the guest's strength; predefined host effects play at the host's own strength.
    // Returns the expected length in milliseconds.
    private static long effect(Vibrator vibrator,int id,int strength) {
        if(id<0 || id>5 || strength<0 || strength>2)throw new IllegalArgumentException("Invalid vibration effect");
        if(Build.VERSION.SDK_INT>=31) {
            int primitive=id==2?VibrationEffect.Composition.PRIMITIVE_TICK:id==3?VibrationEffect.Composition.PRIMITIVE_THUD:
                id==4?-1:VibrationEffect.Composition.PRIMITIVE_CLICK;
            if(primitive>=0 && vibrator.areAllPrimitivesSupported(primitive)) {
                float scale=(id==5?new float[]{0.8f,1f,1f}:new float[]{0.5f,0.75f,1f})[strength];
                VibrationEffect.Composition composition=VibrationEffect.startComposition().addPrimitive(primitive,scale);
                long length=vibrator.getPrimitiveDurations(primitive)[0];
                if(id==1){composition.addPrimitive(primitive,scale,100);length=2*length+100;}
                vibrator.vibrate(composition.compose());
                return Math.max(1,length);
            }
        }
        if(Build.VERSION.SDK_INT>=29 && (id<=2 || id==5)) {
            vibrator.vibrate(VibrationEffect.createPredefined(id));
            // The host does not report predefined durations; these are estimates for the guest's bookkeeping.
            return new long[]{20,120,10,0,0,30}[id];
        }
        throw new UnsupportedOperationException("Host has no matching haptic effect");
    }
    private JSONObject phoneUi(JSONObject args) throws Exception {
        String action=args.getString("action"),address=args.optString("address"),body=args.optString("body");
        if(address.length()>256 || body.length()>8192 || !address.matches("[0-9+*#().,; -]*"))
            throw new IllegalArgumentException("Invalid phone address");
        Intent intent;
        if(action.equals("dial"))intent=new Intent(Intent.ACTION_DIAL,Uri.fromParts("tel",address,null));
        else if(action.equals("sms"))intent=new Intent(Intent.ACTION_SENDTO,Uri.fromParts("smsto",address,null)).putExtra("sms_body",body);
        else throw new IllegalArgumentException("Unsupported phone UI action");
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        if(context.getPackageManager().resolveActivity(intent,0)==null)throw new UnsupportedOperationException("Host has no app for this phone action");
        CountDownLatch done=new CountDownLatch(1);final Exception[] failed={null};
        new Handler(Looper.getMainLooper()).post(()->{
            try {if(closed)throw new IOException("Guest session ended");context.startActivity(intent);}
            catch(Exception error){failed[0]=error;}finally{done.countDown();}
        });
        if(!done.await(5,TimeUnit.SECONDS))throw new IOException("Host phone UI did not respond");
        if(failed[0]!=null)throw failed[0];
        // ACTION_DIAL/SENDTO opens a system UI. Never places a call or sends SMS.
        return new JSONObject().put("opened",true).put("owner","host").put("user_confirmation_required",true);
    }
    private JSONObject contacts() throws Exception {
        JSONArray rows=new JSONArray(),contacts=new JSONArray();boolean truncated=false;
        try(Cursor c=context.getContentResolver().query(ContactsContract.Contacts.CONTENT_URI,
                new String[]{ContactsContract.Contacts._ID,ContactsContract.Contacts.DISPLAY_NAME},null,null,
                ContactsContract.Contacts._ID+" ASC")) {
            if(c==null)throw new IOException("Host contacts provider unavailable");
            while(c.moveToNext()) {
                if(contacts.length()>=10000){truncated=true;break;}
                contacts.put(new JSONObject().put("contact_id",c.getString(0)).put("name",c.isNull(1)?"":c.getString(1)));
            }
        }
        String[] columns={ContactsContract.Data.CONTACT_ID,ContactsContract.Data.DISPLAY_NAME,
            ContactsContract.Data.MIMETYPE,ContactsContract.Data.DATA1,ContactsContract.Data.DATA2};
        String where=ContactsContract.Data.MIMETYPE+" IN (?,?)";
        String[] types={ContactsContract.CommonDataKinds.Phone.CONTENT_ITEM_TYPE,ContactsContract.CommonDataKinds.Email.CONTENT_ITEM_TYPE};
        try(Cursor c=context.getContentResolver().query(ContactsContract.Data.CONTENT_URI,columns,where,types,ContactsContract.Data.CONTACT_ID+" ASC")) {
            if(c==null)throw new IOException("Host contacts provider unavailable");
            while(c.moveToNext()) {
                if(rows.length()>=10000){truncated=true;break;}
                rows.put(new JSONObject().put("contact_id",c.getString(0)).put("name",c.getString(1))
                    .put("mimetype",c.getString(2)).put("value",c.getString(3)).put("type",c.getInt(4)));
            }
        }
        return new JSONObject().put("contacts",contacts).put("rows",rows).put("truncated",truncated).put("source","host_contacts_provider");
    }
    private JSONObject location() throws Exception {
        if(!permitted(context,Manifest.permission.ACCESS_FINE_LOCATION) && !permitted(context,Manifest.permission.ACCESS_COARSE_LOCATION))
            throw new SecurityException("Host location permission required");
        LocationManager lm=(LocationManager)context.getSystemService(Context.LOCATION_SERVICE);
        Location latest=null;
        for(String provider:lm.getProviders(true)) {
            try {Location l=lm.getLastKnownLocation(provider);if(l!=null && (latest==null || l.getElapsedRealtimeNanos()>latest.getElapsedRealtimeNanos()))latest=l;}
            catch(SecurityException denied){ /* A coarse grant does not authorize every provider. */ }
        }
        if(latest==null)throw new UnsupportedOperationException("Host has no known location fix");
        return new JSONObject().put("latitude",latest.getLatitude()).put("longitude",latest.getLongitude())
            .put("provider",latest.getProvider()).put("time_ms",latest.getTime())
            .put("elapsed_realtime_ns",latest.getElapsedRealtimeNanos()).put("accuracy_m",latest.getAccuracy())
            .put("age_ms",Math.max(0,(SystemClock.elapsedRealtimeNanos()-latest.getElapsedRealtimeNanos())/1000000))
            .put("mock",latest.isFromMockProvider());
    }
    interface Reading { Object get() throws Exception; }
    private static void reading(JSONObject result,String key,Reading value) throws Exception {
        try {result.put(key,value.get());}
        catch(SecurityException denied){result.put(key,new JSONObject().put("unavailable","permission_denied"));}
        catch(UnsupportedOperationException unavailable){result.put(key,new JSONObject().put("unavailable","host_unsupported"));}
    }
    private JSONObject phone() throws Exception {
        TelephonyManager tm=(TelephonyManager)context.getSystemService(Context.TELEPHONY_SERVICE);
        JSONObject result=new JSONObject().put("sim_state",tm.getSimState()).put("network_operator",tm.getNetworkOperator())
            .put("network_country_iso",tm.getNetworkCountryIso()).put("sim_operator",tm.getSimOperator())
            .put("phone_type",tm.getPhoneType()).put("ims_owner","host");
        reading(result,"data_network_type",()->tm.getDataNetworkType());
        reading(result,"voice_network_type",()->tm.getVoiceNetworkType());
        reading(result,"call_state",()->tm.getCallState());
        result.put("identifiers",new JSONObject().put("unavailable","not_requested_or_privileged"));
        result.put("ims_registration",new JSONObject().put("unavailable","carrier_or_privileged_access_required"));
        return result;
    }
    private JSONObject sensors(JSONObject args) throws Exception {
        SensorManager manager=(SensorManager)context.getSystemService(Context.SENSOR_SERVICE);
        int type=args.optInt("type",Sensor.TYPE_ACCELEROMETER),wanted=args.optInt("samples",8);
        if(wanted<1 || wanted>128)throw new IllegalArgumentException("Invalid sample count");
        Sensor sensor=manager.getDefaultSensor(type);
        if(sensor==null)throw new UnsupportedOperationException("Requested sensor is absent on the host");
        HandlerThread thread=new HandlerThread("PocketSensorRelay");thread.start();
        JSONArray events=new JSONArray();CountDownLatch ready=new CountDownLatch(wanted);
        SensorEventListener listener=new SensorEventListener() {
            public void onAccuracyChanged(Sensor s,int accuracy) { }
            public void onSensorChanged(SensorEvent event) {
                synchronized(events) {
                    if(events.length()>=wanted)return;
                    try {
                        JSONArray values=new JSONArray();for(float v:event.values)values.put((double)v);
                        events.put(new JSONObject().put("timestamp_ns",event.timestamp).put("accuracy",event.accuracy).put("values",values));
                    }
                    catch(JSONException ignored){return;}
                    ready.countDown();
                }
            }
        };
        try {
            if(!manager.registerListener(listener,sensor,SensorManager.SENSOR_DELAY_GAME,new Handler(thread.getLooper())))
                throw new UnsupportedOperationException("Host refused sensor subscription");
            ready.await(1500,TimeUnit.MILLISECONDS);
        } finally {manager.unregisterListener(listener);thread.quitSafely();thread.join(1000);}
        return new JSONObject().put("type",type).put("name",sensor.getName()).put("vendor",sensor.getVendor())
            .put("events",events).put("requested_samples",wanted).put("source","host_sensor_manager");
    }
    @Override public void close() {
        closed=true;
        try{server.close();}catch(IOException ignored){}
        try{bound.close();}catch(IOException ignored){}
        for(LocalSocket c:clients)try{c.close();}catch(IOException ignored){}
        workers.shutdownNow();endpoint.delete();
    }
}
