// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.pocketdisplaytest;

import android.app.Activity;
import android.graphics.*;
import android.os.*;
import android.util.*;
import android.view.*;
import android.widget.*;
import org.json.JSONObject;

/** Ordinary app APIs verify guest pixels/density independently of the broker. */
public final class MainActivity extends Activity {
    private int taps;
    private void report() {
        DisplayMetrics metrics=new DisplayMetrics();getWindowManager().getDefaultDisplay().getRealMetrics(metrics);
        try {Log.i("PocketDisplayTest",new JSONObject().put("width",metrics.widthPixels).put("height",metrics.heightPixels)
            .put("density_dpi",metrics.densityDpi).put("resource_density_dpi",getResources().getDisplayMetrics().densityDpi)
            .put("guest_uid",android.os.Process.myUid()).put("taps",taps).toString());}
        catch(Exception error){throw new IllegalStateException(error);}
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);if(state!=null)taps=state.getInt("taps");
        LinearLayout layout=new LinearLayout(this);layout.setOrientation(LinearLayout.VERTICAL);layout.setBackgroundColor(Color.WHITE);
        DisplayMetrics metrics=new DisplayMetrics();getWindowManager().getDefaultDisplay().getRealMetrics(metrics);
        TextView title=new TextView(this);title.setTextColor(Color.BLACK);title.setTextSize(20);title.setLines(3);
        title.setText("Native guest display\n"+metrics.widthPixels+" × "+metrics.heightPixels+" · "+metrics.densityDpi+" dpi");layout.addView(title);
        Button button=new Button(this);button.setText("Guest taps: "+taps);layout.addView(button);
        button.setOnClickListener(v->{button.setText("Guest taps: "+(++taps));report();});
        button.addOnLayoutChangeListener((v,l,t,r,b,ol,ot,or,ob)->{
            int[] point=new int[2];button.getLocationOnScreen(point);
            Log.i("PocketDisplayTest","button="+(point[0]+button.getWidth()/2)+","+(point[1]+button.getHeight()/2));
        });
        for(int color:new int[]{Color.RED,Color.GREEN,Color.BLUE}) {
            View bar=new View(this);bar.setBackgroundColor(color);
            layout.addView(bar,new LinearLayout.LayoutParams(-1,Math.round(40*metrics.density)));
        }
        View circle=new View(this) {
            @Override protected void onDraw(Canvas canvas) {
                Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);paint.setColor(Color.BLACK);paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(4*getResources().getDisplayMetrics().density);
                canvas.drawCircle(getWidth()/2f,getHeight()/2f,Math.min(getWidth(),getHeight())/3f,paint);
            }
        };
        layout.addView(circle,new LinearLayout.LayoutParams(-1,Math.round(140*metrics.density)));
        setContentView(layout);report();
    }
    @Override public void onSaveInstanceState(Bundle state){state.putInt("taps",taps);super.onSaveInstanceState(state);}
    @Override public void onBackPressed(){Log.i("PocketDisplayTest","BACK_RECEIVED");super.onBackPressed();}
}
