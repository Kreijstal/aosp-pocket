// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.app.ActivityManager;
import android.content.*;
import android.graphics.Color;
import android.os.Bundle;
import android.view.*;
import android.widget.*;
import dev.kreijstal.aosppocket.bridge.DisplayGeometry;

/** Display page: the guest card and page navigation, with management on Settings. */
public final class MainActivity extends RuntimeActivity {
    private ScreenCard card;
    private LinearLayout idle;
    private Button start;
    @Override public void onCreate(Bundle state) {
        setTheme(android.R.style.Theme_Material_Light_NoActionBar);
        super.onCreate(state);
        autoStart=getIntent().getBooleanExtra("start_guest",false);
        setTaskDescription(new ActivityManager.TaskDescription("Pocket Manager",null,0xff16746e));
        LinearLayout layout=new LinearLayout(this); layout.setOrientation(LinearLayout.VERTICAL);
        FrameLayout page=new FrameLayout(this); page.setBackgroundColor(0xffeef1f4);
        page.setContentDescription("Android page"); page.setPadding(dp(8),dp(8),dp(8),dp(8));
        card=new ScreenCard(); card.setContentDescription("Android screen card");
        display=new GuestDisplay(this,false,true);
        card.addView(display,new FrameLayout.LayoutParams(-1,-1));
        Button expand=new Button(this); expand.setText("⛶"); expand.setTextSize(23); expand.setPadding(0,0,0,0);
        expand.setContentDescription("Full screen"); expand.setOnClickListener(v->startActivity(GuestActivity.intent(this)));
        card.addView(expand,new FrameLayout.LayoutParams(dp(40),dp(40),Gravity.TOP|Gravity.RIGHT));
        page.addView(card,new FrameLayout.LayoutParams(-1,-1,Gravity.CENTER));
        idle=new LinearLayout(this); idle.setOrientation(LinearLayout.VERTICAL); idle.setGravity(Gravity.CENTER);
        status=new TextView(this); status.setGravity(Gravity.CENTER); status.setTextColor(0xff33414a);
        idle.addView(status,new LinearLayout.LayoutParams(-1,-2));
        start=new Button(this); start.setText("Start Android"); start.setOnClickListener(v->requestStart());
        idle.addView(start,new LinearLayout.LayoutParams(-2,dp(48)));
        page.addView(idle,new FrameLayout.LayoutParams(-1,-1));
        layout.addView(page,new LinearLayout.LayoutParams(-1,0,1));
        LinearLayout navigation=new LinearLayout(this); navigation.setBackgroundColor(Color.WHITE);
        Button androidTab=button("Android",navigation,v->{}); androidTab.setSelected(true); androidTab.setEnabled(false);
        button("Settings",navigation,v->startActivity(new Intent(this,SettingsActivity.class)));
        layout.addView(navigation,new LinearLayout.LayoutParams(-1,dp(48)));
        setContentView(layout); onGuestChanged();
    }
    @Override public void onGuestChanged() {
        super.onGuestChanged(); if (card==null) return;
        if (runtime!=null) card.geometry(runtime.width,runtime.height);
        boolean ready=runtime!=null && runtime.booted();
        card.setVisibility(ready ? View.VISIBLE : View.GONE);
        idle.setVisibility(ready ? View.GONE : View.VISIBLE);
        boolean busy=runtime!=null && (runtime.starting() || runtime.preparing() || runtime.stopping());
        status.setText(runtime==null ? "Connecting to Android…" : busy ? "Starting Android…" : "Your Android is ready");
        start.setVisibility(busy || ready ? View.GONE : View.VISIBLE);
        start.setEnabled(runtime!=null && runtime.canStart() && !runtime.running() && !busy);
    }
    /** Fit the container once; the guest Surface then fills it edge to edge. */
    private final class ScreenCard extends FrameLayout {
        private int width=1,height=1;
        ScreenCard() { super(MainActivity.this); }
        void geometry(int w,int h) { if(width!=w || height!=h){width=w;height=h;requestLayout();} }
        @Override protected void onMeasure(int widthSpec,int heightSpec) {
            int[] fit=new DisplayGeometry(width,height,160).fit(MeasureSpec.getSize(widthSpec),MeasureSpec.getSize(heightSpec));
            super.onMeasure(MeasureSpec.makeMeasureSpec(fit[0],MeasureSpec.EXACTLY),MeasureSpec.makeMeasureSpec(fit[1],MeasureSpec.EXACTLY));
        }
    }
}
