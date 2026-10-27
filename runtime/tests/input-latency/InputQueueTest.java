// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.app.Activity;
import android.net.*;
import android.os.*;
import android.view.*;
import android.widget.TextView;
import java.io.*;
import java.lang.reflect.Field;
import java.util.*;
import java.util.concurrent.*;
import dev.kreijstal.aosppocket.bridge.DisplayGeometry;
import org.json.*;

/** Production host touch queue, a real socket, and deterministic backpressure. */
public final class InputQueueTest extends Activity {
    private static final DisplayGeometry GEOMETRY=new DisplayGeometry(720,1280,240);
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text=new TextView(this);text.setText("Testing pending motion and input boundaries…");setContentView(text);
        new Thread(()->{
            JSONObject report;
            try {report=check();}
            catch(Throwable error) {report=new JSONObject();try{report.put("error",error.toString());}catch(JSONException ignored){}}
            try(FileOutputStream out=openFileOutput("result.json",0)){out.write(report.toString().getBytes("UTF-8"));}
            catch(IOException error){throw new RuntimeException(error);}
            final String result=report.toString();runOnUiThread(()->text.setText(result));
        },"InputQueueTest").start();
    }
    private static void touch(DirectSession session,long down,int action,int[] ids,float x) {
        MotionEvent.PointerProperties[] properties=new MotionEvent.PointerProperties[ids.length];
        MotionEvent.PointerCoords[] coords=new MotionEvent.PointerCoords[ids.length];
        for(int i=0;i<ids.length;i++) {
            properties[i]=new MotionEvent.PointerProperties();properties[i].id=ids[i];properties[i].toolType=MotionEvent.TOOL_TYPE_FINGER;
            coords[i]=new MotionEvent.PointerCoords();coords[i].x=x+i*50;coords[i].y=100;coords[i].pressure=1;
        }
        MotionEvent event=MotionEvent.obtain(down,SystemClock.uptimeMillis(),action,ids.length,properties,coords,0,0,1,1,0,0,InputDevice.SOURCE_TOUCHSCREEN,0);
        try{session.touch(event,1,1);}finally{event.recycle();}
    }
    private JSONObject check()throws Exception {
        File root=new File(getFilesDir(),"queue");new File(root,"dev/socket").mkdirs();
        File endpoint=new File(root,"dev/socket/pocket_control");endpoint.delete();
        LocalSocket bound=new LocalSocket();bound.bind(new LocalSocketAddress(endpoint.getPath(),LocalSocketAddress.Namespace.FILESYSTEM));
        LocalServerSocket server=new LocalServerSocket(bound.getFileDescriptor());
        CountDownLatch blocked=new CountDownLatch(1),release=new CountDownLatch(1),done=new CountDownLatch(1);
        List<Integer> boundaries=new CopyOnWriteArrayList<>();
        List<String> errors=new CopyOnWriteArrayList<>();
        long[] times=new long[2];int[] moves={0};float[] finalFirstMotion={-1};
        List<String> motionEnds=new CopyOnWriteArrayList<>();
        Thread peer=new Thread(()->{
            try(LocalSocket socket=server.accept()) {
                socket.setSoTimeout(15000);
                DataInputStream in=new DataInputStream(socket.getInputStream());DataOutputStream out=new DataOutputStream(socket.getOutputStream());
                GEOMETRY.write(out);out.flush();boolean first=true,firstGesture=true;float previousMove=-1;
                while(true) {
                    int op=in.readInt();boolean end=false;
                    if(op==2) {
                        in.readLong();in.readLong();int action=in.readInt(),count=in.readInt();int[] ids=new int[count];float x=0;
                        for(int i=0;i<count;i++){ids[i]=in.readInt();float next=in.readFloat();in.readFloat();if(i==0)x=next;}
                        if((action&255)==MotionEvent.ACTION_MOVE) {
                            previousMove=x;
                            if(firstGesture){moves[0]++;finalFirstMotion[0]=x;}
                        } else {
                            boundaries.add(action);
                            if(previousMove>=0){motionEnds.add(count+":"+previousMove);previousMove=-1;}
                        }
                        if((action&255)==MotionEvent.ACTION_POINTER_DOWN && (count!=2 || ids[0]!=0 || ids[1]!=7))throw new AssertionError("Pointer identity changed");
                        if(firstGesture && action==MotionEvent.ACTION_UP){times[1]=SystemClock.elapsedRealtimeNanos();firstGesture=false;}
                    } else if(op==3) {
                        int key=in.readInt();boundaries.add(-key);end=key==KeyEvent.KEYCODE_BACK;
                    } else throw new IOException("Unexpected operation "+op);
                    if(first){first=false;blocked.countDown();release.await();}
                    // Force a busy transport. This is a queue regression test,
                    // not a measurement of Android's normal socket latency.
                    Thread.sleep(10);out.writeBoolean(true);out.flush();
                    if(end)break;
                }
            } catch(Throwable error){errors.add(error.toString());}
            finally {done.countDown();}
        },"SlowInputPeer");peer.start();
        DirectSession session=new DirectSession(this,GEOMETRY,60,new DirectSession.Listener(){
            public void status(String value){errors.add(value);}public void ready(){}public void stopped(String value){}
        });
        for(String name:new String[]{"root","ready"}) {
            Field field=DirectSession.class.getDeclaredField(name);field.setAccessible(true);
            if(name.equals("root"))field.set(session,root);else field.setBoolean(session,true);
        }
        try {
            long down=SystemClock.uptimeMillis();int[] one={0},two={0,7};
            touch(session,down,MotionEvent.ACTION_DOWN,one,0);
            if(!blocked.await(10,TimeUnit.SECONDS))throw new AssertionError("Input did not reach gated peer");
            for(int i=1;i<=80;i++)touch(session,down,MotionEvent.ACTION_MOVE,one,i);
            touch(session,down,MotionEvent.ACTION_UP,one,80);session.key(KeyEvent.KEYCODE_HOME);
            down=SystemClock.uptimeMillis();touch(session,down,MotionEvent.ACTION_DOWN,one,100);
            touch(session,down,MotionEvent.ACTION_MOVE,one,110);
            touch(session,down,MotionEvent.ACTION_POINTER_DOWN|(1<<8),two,110);
            for(int i=111;i<=120;i++)touch(session,down,MotionEvent.ACTION_MOVE,two,i);
            touch(session,down,MotionEvent.ACTION_POINTER_UP|(1<<8),two,120);
            touch(session,down,MotionEvent.ACTION_MOVE,one,130);touch(session,down,MotionEvent.ACTION_CANCEL,one,130);
            for(int i=0;i<10;i++) {
                down=SystemClock.uptimeMillis();touch(session,down,MotionEvent.ACTION_DOWN,one,200+i);
                touch(session,down,MotionEvent.ACTION_UP,one,200+i);
            }
            session.key(KeyEvent.KEYCODE_BACK);times[0]=SystemClock.elapsedRealtimeNanos();release.countDown();
            if(!done.await(15,TimeUnit.SECONDS))throw new AssertionError("Input queue did not drain");
            List<Integer> expected=new ArrayList<>(Arrays.asList(0,1,-KeyEvent.KEYCODE_HOME,0,261,262,3));
            for(int i=0;i<10;i++){expected.add(0);expected.add(1);}expected.add(-KeyEvent.KEYCODE_BACK);
            boolean preserved=expected.equals(boundaries) && motionEnds.equals(Arrays.asList("1:80.0","2:110.0","2:120.0","1:130.0")) && errors.isEmpty();
            return new JSONObject().put("validation",preserved?"PASS":"FAIL")
                    .put("classification","synthetic backpressure; production DirectSession and real LocalSocket")
                    .put("queuedFirstGestureMoves",80).put("deliveredFirstGestureMoves",moves[0])
                    .put("latestFirstGestureX",finalFirstMotion[0]).put("releaseDelayAfterUnblockMs",(times[1]-times[0])/1000000.0)
                    .put("boundariesPreserved",preserved).put("boundaries",new JSONArray(boundaries))
                    .put("motionBeforeBoundaries",new JSONArray(motionEnds)).put("errors",new JSONArray(errors));
        } finally {
            release.countDown();session.close();server.close();bound.close();peer.join(2000);endpoint.delete();
        }
    }
}
