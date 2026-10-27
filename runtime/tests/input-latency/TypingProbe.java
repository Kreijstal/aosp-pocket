// SPDX-License-Identifier: GPL-3.0-only
import android.app.UiAutomation;
import android.accessibilityservice.AccessibilityServiceInfo;
import android.graphics.Rect;
import android.net.*;
import android.os.*;
import android.view.accessibility.*;
import java.io.*;
import java.util.*;
import org.json.*;
import dev.kreijstal.aosppocket.bridge.DisplayGeometry;

/** Tap real LatinIME number keys via the production socket; time real app edits. */
public final class TypingProbe {
    private static DataInputStream snapshotsIn;
    private static DataOutputStream snapshotsOut;
    private static void keys(AccessibilityNodeInfo node,Map<Character,Rect> found,StringBuilder trace) {
        if(node==null)return;
        CharSequence raw=node.getContentDescription();if(raw==null)raw=node.getText();
        String label=raw==null?"":raw.toString();Rect bounds=new Rect();node.getBoundsInScreen(bounds);
        if(trace.length()<5000)trace.append(label).append(' ').append(bounds).append('\n');
        if(label.length()>0 && label.charAt(0)>='0' && label.charAt(0)<='9'
                && !bounds.isEmpty() && (label.length()==1 || !Character.isDigit(label.charAt(1))))
            found.put(label.charAt(0),bounds);
        for(int i=0;i<node.getChildCount();i++) {
            AccessibilityNodeInfo child=node.getChild(i);
            try{keys(child,found,trace);}finally{if(child!=null)child.recycle();}
        }
    }
    private static JSONObject waitText(String file,String expected,long afterNs)throws Exception {
        long deadline=SystemClock.uptimeMillis()+20000;
        JSONObject last=null;
        while(SystemClock.uptimeMillis()<deadline) {
            try {
                snapshotsOut.writeInt(0);snapshotsOut.flush();
                JSONObject response=new JSONObject(snapshotsIn.readUTF());
                last=response;
                JSONObject result=response.getJSONObject(file.equals("typing-change.json")?"edit":"draw");
                if(result.getString("text").equals(expected) && result.getLong("atNs")>=afterNs)return result;
            } catch(IOException|JSONException pending) { }
            Thread.sleep(10);
        }
        snapshotsOut.writeInt(1);snapshotsOut.flush();last=new JSONObject(snapshotsIn.readUTF());
        throw new AssertionError("Expected numeric keyboard edit/draw did not arrive: "+file+" expected="+expected+" last="+last);
    }
    private static void run()throws Exception {
        Class<?> connection=Class.forName("android.app.IUiAutomationConnection");
        Object owner=Class.forName("android.app.UiAutomationConnection").getConstructor().newInstance();
        UiAutomation ui=(UiAutomation)UiAutomation.class.getConstructor(Looper.class,connection)
                .newInstance(Looper.getMainLooper(),owner);
        UiAutomation.class.getMethod("connect",int.class).invoke(ui,1);
        boolean connected=true;
        try {
            AccessibilityServiceInfo info=ui.getServiceInfo();
            info.flags|=AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS
                    |AccessibilityServiceInfo.FLAG_INCLUDE_NOT_IMPORTANT_VIEWS|AccessibilityServiceInfo.FLAG_REPORT_VIEW_IDS;
            ui.setServiceInfo(info);
            Map<Character,Rect> found=new HashMap<>();StringBuilder trace=new StringBuilder();
            long deadline=SystemClock.uptimeMillis()+5000;
            while(found.size()<10 && SystemClock.uptimeMillis()<deadline) {
                found.clear();trace.setLength(0);
                for(AccessibilityWindowInfo window:ui.getWindows()) {
                    AccessibilityNodeInfo root=window.getRoot();
                    try {
                        Rect windowBounds=new Rect();window.getBoundsInScreen(windowBounds);
                        trace.append("window type=").append(window.getType()).append(" bounds=").append(windowBounds)
                                .append(" package=").append(root==null?"null":root.getPackageName()).append('\n');
                        if(root!=null && "com.android.inputmethod.latin".contentEquals(root.getPackageName()))keys(root,found,trace);
                    } finally {if(root!=null)root.recycle();window.recycle();}
                }
                if(found.size()<10)Thread.sleep(300);
            }
            if(found.size()<10) {
                android.graphics.Bitmap bitmap=ui.takeScreenshot();
                if(bitmap!=null && bitmap.getConfig()==android.graphics.Bitmap.Config.HARDWARE) {
                    android.graphics.Bitmap readable=bitmap.copy(android.graphics.Bitmap.Config.ARGB_8888,false);
                    bitmap.recycle();bitmap=readable;
                }
                if(bitmap!=null)try {
                    try(FileOutputStream image=new FileOutputStream("/data/local/tmp/typing-keyboard.png")) {
                        bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG,100,image);
                    }
                    // Some Android 9 IME windows expose no accessibility tree.
                    // This fallback is restricted to the visually verified Pi
                    // phone layout. Every injected digit is independently
                    // checked in the app, so a different layout fails closed.
                    int w=bitmap.getWidth(),h=bitmap.getHeight(),sample=h*3/4;
                    if(w==720 && h==1280 && (bitmap.getPixel(w-2,sample)&0xffffff)==0xeceff1) {
                        int top=sample,bottom=sample;
                        while(top>0 && (bitmap.getPixel(w-2,top-1)&0xffffff)==0xeceff1)top--;
                        while(bottom<h-1 && (bitmap.getPixel(w-2,bottom+1)&0xffffff)==0xeceff1)bottom++;
                        if(top>h/2 && bottom>h*85/100 && bottom-top>h/5 && bottom-top<h/2) {
                            for(int digit=0;digit<=9;digit++) {
                                int row=digit==0?3:(digit-1)/3,col=digit==0?1:(digit-1)%3;
                                int x=(int)(w*(col+.5)*4/15.0),y=top+(int)((bottom-top+1)*(row+.5)/4);
                                found.put((char)('0'+digit),new Rect(x-5,y-5,x+5,y+5));
                            }
                        }
                    }
                } finally {bitmap.recycle();}
                if(found.size()<10)throw new AssertionError("Numeric keyboard keys not exposed or verified: "+trace);
            }
            // Accessibility inspection can change LatinIME touch exploration.
            // Restore normal keyboard dispatch before timing ordinary taps.
            UiAutomation.class.getMethod("disconnect").invoke(ui);connected=false;
            Thread.sleep(300);
            measure(found);
        } finally {if(connected)UiAutomation.class.getMethod("disconnect").invoke(ui);}
    }
    private static void measure(Map<Character,Rect> found)throws Exception {
            JSONArray samples=new JSONArray();String text="";
            try(LocalSocket snapshots=new LocalSocket();LocalSocket socket=new LocalSocket()) {
                snapshots.connect(new LocalSocketAddress("pocket-numeric-typing-test",LocalSocketAddress.Namespace.ABSTRACT));
                snapshots.setSoTimeout(5000);snapshotsIn=new DataInputStream(snapshots.getInputStream());
                snapshotsOut=new DataOutputStream(snapshots.getOutputStream());
                snapshotsOut.writeInt(0);snapshotsOut.flush();
                JSONObject initial=new JSONObject(snapshotsIn.readUTF());
                if(!initial.getString("fieldText").isEmpty()
                        || !initial.getJSONObject("edit").getString("text").isEmpty())
                    throw new AssertionError("Typing test requires a freshly started empty editor");
                socket.connect(new LocalSocketAddress("/dev/socket/pocket_control",LocalSocketAddress.Namespace.FILESYSTEM));
                socket.setSoTimeout(15000);DataInputStream in=new DataInputStream(socket.getInputStream());
                DataOutputStream out=new DataOutputStream(socket.getOutputStream());DisplayGeometry size=DisplayGeometry.read(in);
                if(size.width!=720 || size.height!=1280)throw new AssertionError("Typing coordinates require the verified 720x1280 Pi layout");
                // Match normal use: explicitly tap the editor before typing.
                // Programmatic focus alone can leave Android 9's IME attached
                // to the preceding activity while its keyboard stays visible.
                long focusDown=SystemClock.uptimeMillis();
                for(int action:new int[]{0,1}) {
                    out.writeInt(2);out.writeLong(focusDown);out.writeLong(SystemClock.uptimeMillis());
                    out.writeInt(action);out.writeInt(1);out.writeInt(0);out.writeFloat(360);out.writeFloat(190);out.flush();
                    if(!in.readBoolean())throw new IOException("Guest rejected editor focus tap");
                    if(action==0)Thread.sleep(40);
                }
                Thread.sleep(500);
                for(char digit:"1234567890".toCharArray()) {
                    Rect bounds=found.get(digit);long began=SystemClock.elapsedRealtimeNanos(),down=SystemClock.uptimeMillis();
                    for(int action:new int[]{0,1}) {
                        out.writeInt(2);out.writeLong(down);out.writeLong(SystemClock.uptimeMillis());
                        out.writeInt(action);out.writeInt(1);out.writeInt(0);
                        out.writeFloat(bounds.exactCenterX());out.writeFloat(bounds.exactCenterY());out.flush();
                        if(!in.readBoolean())throw new IOException("Guest rejected numeric key tap");
                        if(action==0)Thread.sleep(40);
                    }
                    text+=digit;JSONObject edited=waitText("typing-change.json",text,began),drawn=waitText("typing-draw.json",text,began);
                    samples.put(new JSONObject().put("digit",String.valueOf(digit))
                            .put("editMs",(edited.getLong("atNs")-began)/1000000.0)
                            .put("drawSubmissionMs",(drawn.getLong("atNs")-began)/1000000.0));
                    Thread.sleep(80);
                }
            }
            System.out.println("TYPING_RESULT "+new JSONObject().put("validation","PASS").put("samples",samples)
                    .put("text",text).put("measurement","real numeric IME tap to app text mutation and draw submission; excludes final presentation"));
    }
    public static void main(String[] args) {
        try {
            if(args.length>0 && args[0].equals("--direct")) {
                Map<Character,Rect> found=new HashMap<>();
                for(int digit=0;digit<=9;digit++) {
                    int row=digit==0?3:(digit-1)/3,col=digit==0?1:(digit-1)%3;
                    int x=96+192*col,y=822+110*row;found.put((char)('0'+digit),new Rect(x-5,y-5,x+5,y+5));
                }
                measure(found);System.exit(0);
            }
            android.system.Os.setgid(1000);android.system.Os.setuid(1000);Looper.prepareMainLooper();
            Class.forName("android.app.ActivityThread").getMethod("systemMain").invoke(null);
            final Throwable[] failure={null};Thread worker=new Thread(()->{try{run();}catch(Throwable error){failure[0]=error;}},"TypingProbe");
            worker.start();worker.join(180000);if(worker.isAlive())throw new AssertionError("Typing probe timed out");
            if(failure[0]!=null)throw failure[0];System.exit(0);
        } catch(Throwable error){error.printStackTrace();System.exit(1);}
    }
}
