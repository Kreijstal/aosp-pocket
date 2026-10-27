// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketinputlatency;

import android.app.Activity;
import android.graphics.Canvas;
import android.os.*;
import android.text.*;
import android.view.*;
import android.view.inputmethod.InputMethodManager;
import android.widget.*;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import java.io.*;
import org.json.JSONObject;

/** Records real numeric IME commits and draw submission, using synthetic digits. */
public final class TypingActivity extends Activity {
    private boolean changed;
    private static volatile EditText activeField;
    private static final class Sample {
        final String text;final long time;
        Sample(String text){this.text=text;this.time=SystemClock.elapsedRealtimeNanos();}
        JSONObject json()throws Exception{return new JSONObject().put("text",text).put("atNs",time);}
    }
    private static volatile Sample lastEdit=new Sample(""),lastDraw=new Sample("");
    private static boolean started;
    private void record(String file,String text) {
        if(file.equals("typing-change.json"))lastEdit=new Sample(text);else lastDraw=new Sample(text);
    }
    private static synchronized void startSnapshots() {
        if(started)return;started=true;
        Thread worker=new Thread(()->{
            try(LocalServerSocket server=new LocalServerSocket("pocket-numeric-typing-test")) {
                while(true)try(LocalSocket peer=server.accept()) {
                    DataInputStream in=new DataInputStream(peer.getInputStream());
                    DataOutputStream out=new DataOutputStream(peer.getOutputStream());
                    while(true) {
                        int operation=in.readInt();if(operation!=0 && operation!=1)break;
                        EditText field=activeField;
                        JSONObject status=new JSONObject().put("edit",lastEdit.json()).put("draw",lastDraw.json())
                                .put("viewFocused",field!=null && field.isFocused())
                                .put("windowFocused",field!=null && field.hasWindowFocus())
                                .put("fieldText",field==null?"":field.getText().toString());
                        if(operation==1) {
                            StringBuilder trace=new StringBuilder();
                            for(StackTraceElement line:Looper.getMainLooper().getThread().getStackTrace())trace.append(line).append('\n');
                            status.put("mainStack",trace.toString());
                            try {
                                Object ime=field.getContext().getSystemService(INPUT_METHOD_SERVICE);
                                status.put("imeIsActive",((InputMethodManager)ime).isActive(field))
                                        .put("imeAcceptingText",((InputMethodManager)ime).isAcceptingText());
                                for(String name:new String[]{"mActive","mServedConnecting","mServedView","mNextServedView"}) {
                                    java.lang.reflect.Field member=InputMethodManager.class.getDeclaredField(name);member.setAccessible(true);
                                    status.put(name,String.valueOf(member.get(ime)));
                                }
                            } catch(Exception unavailable){status.put("imeDiagnosticError",unavailable.toString());}
                        }
                        out.writeUTF(status.toString());out.flush();
                    }
                } catch(EOFException closed) { }
            } catch(Exception failure){android.util.Log.e("TypingTest","Snapshot server failed",failure);}
        },"TypingSnapshots");worker.setDaemon(true);worker.start();
    }
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        lastEdit=new Sample("");lastDraw=new Sample("");startSnapshots();
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setTurnScreenOn(true);
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE
                |WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        LinearLayout content=new LinearLayout(this);content.setOrientation(LinearLayout.VERTICAL);
        TextView title=new TextView(this);title.setText("Numeric keyboard latency test");content.addView(title);
        EditText field=new EditText(this) {
            @Override protected void onDraw(Canvas canvas) {
                super.onDraw(canvas);
                if(changed){changed=false;record("typing-draw.json",getText().toString());}
            }
        };
        field.setSingleLine(true);field.setTextSize(30);field.setInputType(InputType.TYPE_CLASS_PHONE);
        activeField=field;
        field.addTextChangedListener(new TextWatcher() {
            public void beforeTextChanged(CharSequence s,int start,int count,int after) { }
            public void onTextChanged(CharSequence s,int start,int before,int count) { }
            public void afterTextChanged(Editable value){changed=true;record("typing-change.json",value.toString());}
        });
        content.addView(field);setContentView(content);field.requestFocus();
        field.postDelayed(()->((InputMethodManager)getSystemService(INPUT_METHOD_SERVICE))
                .showSoftInput(field,InputMethodManager.SHOW_IMPLICIT),300);
    }
    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if(focused && activeField!=null)activeField.post(()->{
            activeField.requestFocus();InputMethodManager ime=(InputMethodManager)getSystemService(INPUT_METHOD_SERVICE);
            ime.restartInput(activeField);ime.showSoftInput(activeField,InputMethodManager.SHOW_IMPLICIT);
        });
    }
}
