// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.animation.*;
import android.app.*;
import android.content.*;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.graphics.*;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.util.Rational;
import android.view.*;
import android.view.animation.DecelerateInterpolator;
import android.widget.*;

/** One immersive guest task, with an in-window control handle instead of a toolbar. */
public final class GuestActivity extends RuntimeActivity {
    private FrameLayout content;
    private Button handle;
    private PopupMenu menu;
    private Reveal transition;
    private boolean entering = true, leaving, rightEdge = true;
    private float vertical = .35f;
    static Intent intent(Context context) {
        return new Intent(context, GuestActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        autoStart = true;
        if (android.os.Build.VERSION.SDK_INT>=33) BackApi33.register(this);
        rightEdge = getPreferences(MODE_PRIVATE).getBoolean("handle-right", true);
        vertical = getPreferences(MODE_PRIVATE).getFloat("handle-y", .35f);
        if (android.os.Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams attributes = getWindow().getAttributes();
            attributes.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(attributes);
        }
        setTaskDescription(new ActivityManager.TaskDescription("Pocket Android", null, 0xff16746e));
        content = new FrameLayout(this); content.setBackgroundColor(Color.BLACK);
        display = new GuestDisplay(this, true);
        content.addView(display, new FrameLayout.LayoutParams(-1, -1));
        status = new TextView(this); status.setTextColor(Color.WHITE); status.setBackgroundColor(0xaa000000);
        content.addView(status, new FrameLayout.LayoutParams(-1, -2));
        handle = new Button(this) {
            private float downX, downY, startX, startY;
            private boolean dragging;
            @Override public boolean onTouchEvent(MotionEvent event) {
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        downX=event.getRawX(); downY=event.getRawY(); startX=getX(); startY=getY(); dragging=false;
                        setPressed(true); return true;
                    case MotionEvent.ACTION_MOVE:
                        float dx=event.getRawX()-downX, dy=event.getRawY()-downY;
                        if (Math.hypot(dx,dy)>ViewConfiguration.get(getContext()).getScaledTouchSlop()) dragging=true;
                        if (dragging) { setX(clamp(startX+dx,content.getWidth()-getWidth())); setY(clamp(startY+dy,content.getHeight()-getHeight())); }
                        return true;
                    case MotionEvent.ACTION_UP:
                        setPressed(false);
                        if (!dragging) performClick();
                        else {
                            rightEdge=getX()+getWidth()/2f>=content.getWidth()/2f;
                            vertical=getY()/Math.max(1,content.getHeight()-getHeight());
                            getPreferences(MODE_PRIVATE).edit().putBoolean("handle-right",rightEdge).putFloat("handle-y",vertical).apply();
                            positionHandle();
                        }
                        return true;
                    case MotionEvent.ACTION_CANCEL: setPressed(false); positionHandle(); return true;
                    default: return true;
                }
            }
            @Override public boolean performClick() { return super.performClick(); }
        };
        handle.setText("⋮"); handle.setTextSize(24); handle.setTextColor(Color.WHITE);
        handle.setContentDescription("Android controls"); handle.setPadding(0,0,0,0); handle.setElevation(dp(6));
        GradientDrawable background=new GradientDrawable(); background.setShape(GradientDrawable.OVAL);
        background.setColor(0xc016746e); background.setStroke(dp(1),0x99ffffff); handle.setBackground(background);
        handle.setOnClickListener(v -> showControls());
        content.addView(handle,new FrameLayout.LayoutParams(dp(48),dp(48),Gravity.TOP|Gravity.LEFT));
        content.addOnLayoutChangeListener((v,l,t,r,b,ol,ot,or,ob)->positionHandle());
        transition = new Reveal(); content.addView(transition,new FrameLayout.LayoutParams(-1,-1));
        setContentView(content); immersive(); onGuestChanged();
    }
    private static float clamp(float value,int limit) { return Math.max(0,Math.min(Math.max(0,limit),value)); }
    private void positionHandle() {
        if (handle==null) return;
        handle.setX(rightEdge ? Math.max(0,content.getWidth()-handle.getWidth()-dp(8)) : Math.min(dp(8),Math.max(0,content.getWidth()-handle.getWidth())));
        handle.setY(clamp(vertical*(content.getHeight()-handle.getHeight()),content.getHeight()-handle.getHeight()));
    }
    private void immersive() {
        if (!isInPictureInPictureMode()) getWindow().getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN |
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }
    private void showControls() {
        if (menu!=null || isInPictureInPictureMode()) return;
        menu=new PopupMenu(this,handle);
        for (String title:new String[]{"Back","Home","Recents","Manager","Float","Close window"}) menu.getMenu().add(title);
        boolean navigation=runtime!=null && runtime.directBackend() && runtime.booted();
        for (int i=0;i<3;i++) menu.getMenu().getItem(i).setEnabled(navigation);
        menu.getMenu().getItem(4).setEnabled(canFloat());
        menu.setOnMenuItemClickListener(item->{
            String title=item.getTitle().toString();
            if (title.equals("Manager")) returnToManager();
            else if (title.equals("Float")) enterFloating();
            else if (title.equals("Close window")) { leaving=true; reveal(false,()->finishAndRemoveTask()); }
            else if (runtime!=null) runtime.key(title.equals("Back") ? KeyEvent.KEYCODE_BACK : title.equals("Home") ? KeyEvent.KEYCODE_HOME : KeyEvent.KEYCODE_APP_SWITCH);
            return true;
        });
        menu.setOnDismissListener(p->{menu=null;immersive();}); menu.show();
    }
    private void dismissControls() { if (menu!=null) menu.dismiss(); }
    private void returnToManager() {
        leaving=true;
        reveal(false,()->{
            startActivity(new Intent(this,MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK|Intent.FLAG_ACTIVITY_SINGLE_TOP|Intent.FLAG_ACTIVITY_NO_USER_ACTION));
            finishAndRemoveTask();
        });
    }
    @Override public void onBackPressed() {
        if (menu!=null) { dismissControls(); return; }
        if (runtime!=null && runtime.directBackend() && runtime.booted()) {
            runtime.key(KeyEvent.KEYCODE_BACK); android.util.Log.i("PocketWindows","Host Back forwarded to guest");
        } else returnToManager();
    }
    /** Keep platform Back classes out of the Android 9 host's class loading path. */
    private static final class BackApi33 {
        static void register(GuestActivity activity) {
            activity.getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT,activity::onBackPressed);
        }
    }
    @Override protected void onStart() { autoStart=true; super.onStart(); }
    @Override protected void onResume() { super.onResume(); leaving=false; }
    @Override protected void onStop() { dismissControls(); cancelReveal(); super.onStop(); }
    @Override public void onWindowFocusChanged(boolean focus) { super.onWindowFocusChanged(focus); if (focus) immersive(); }
    @Override public void onGuestChanged() {
        super.onGuestChanged();
        if (status!=null) status.setVisibility(isInPictureInPictureMode() || runtime!=null && runtime.booted() ? View.GONE : View.VISIBLE);
        if (entering && runtime!=null && runtime.booted() && transition!=null) {
            entering=false; content.post(()->{ if (!isFinishing() && !isInPictureInPictureMode()) reveal(true,null); });
        }
    }
    private boolean canFloat() {
        return runtime!=null && runtime.running() && getPackageManager().hasSystemFeature(PackageManager.FEATURE_PICTURE_IN_PICTURE);
    }
    private void enterFloating() {
        if (!canFloat()) return;
        cancelReveal(); dismissControls();
        int w=runtime.width,h=runtime.height;
        if ((long)w*100>239L*h) w=h*239/100;
        if ((long)h*100>239L*w) h=w*239/100;
        PictureInPictureParams.Builder params=new PictureInPictureParams.Builder()
            .setAspectRatio(new Rational(w,h)).setSourceRectHint(display.contentBounds());
        if (android.os.Build.VERSION.SDK_INT>=31) params.setSeamlessResizeEnabled(false);
        try {
            if (!enterPictureInPictureMode(params.build())) Toast.makeText(this,"Floating window is unavailable on this host",Toast.LENGTH_SHORT).show();
        } catch (IllegalStateException unavailable) { android.util.Log.w("PocketWindows","PiP unavailable",unavailable); }
    }
    @Override protected void onUserLeaveHint() {
        super.onUserLeaveHint();
        if (!leaving && !isInPictureInPictureMode() && canFloat()) enterFloating();
    }
    @Override public void onPictureInPictureModeChanged(boolean pip,Configuration configuration) {
        super.onPictureInPictureModeChanged(pip,configuration);
        android.util.Log.i("PocketWindows","Guest PiP="+pip+" task="+getTaskId());
        handle.setVisibility(pip ? View.GONE : View.VISIBLE); onGuestChanged();
        if (!pip) immersive();
    }
    private void cancelReveal() {
        if (transition!=null) transition.cancel();
        if (display!=null) display.setVisibility(View.VISIBLE);
        if (handle!=null) handle.setVisibility(isInPictureInPictureMode() ? View.GONE : View.VISIBLE);
    }
    private void reveal(boolean enter,Runnable completion) {
        cancelReveal();
        if (runtime==null || !runtime.directBackend() || content.getWidth()==0) {
            if (completion!=null) completion.run(); return;
        }
        Bitmap copy=runtime.snapshot();
        if (copy==null) { if (completion!=null) completion.run(); return; }
        display.setVisibility(View.INVISIBLE); handle.setVisibility(View.INVISIBLE);
        transition.start(copy,enter,()->{cancelReveal(); if (completion!=null) completion.run();});
    }
    /** Snapshot transition also works with the software host window renderer. */
    private final class Reveal extends View {
        private Bitmap snapshot;
        private ValueAnimator animator;
        private float fraction;
        private final Paint paint=new Paint(Paint.FILTER_BITMAP_FLAG);
        Reveal() { super(GuestActivity.this); setVisibility(GONE); }
        void start(Bitmap image,boolean enter,Runnable completion) {
            snapshot=image; fraction=enter ? 0 : 1; setVisibility(VISIBLE);
            animator=ValueAnimator.ofFloat(fraction,enter ? 1 : 0); animator.setDuration(300);
            animator.setInterpolator(new DecelerateInterpolator());
            animator.addUpdateListener(a->{fraction=(Float)a.getAnimatedValue();invalidate();});
            animator.addListener(new AnimatorListenerAdapter(){@Override public void onAnimationEnd(Animator a){completion.run();}});
            animator.start();
        }
        void cancel() {
            if (animator!=null) { animator.removeAllListeners(); animator.cancel(); animator=null; }
            setVisibility(GONE); if (snapshot!=null) {snapshot.recycle();snapshot=null;}
        }
        @Override protected void onDraw(Canvas canvas) {
            canvas.drawColor(Color.BLACK); if (snapshot==null) return;
            int saved=canvas.save(); Path path=new Path();
            path.addCircle(getWidth()/2f,getHeight()/2f,(float)Math.hypot(getWidth()/2f,getHeight()/2f)*fraction,Path.Direction.CW);
            canvas.clipPath(path);
            int[] fit=new dev.kreijstal.aosppocket.bridge.DisplayGeometry(snapshot.getWidth(),snapshot.getHeight(),160).fit(getWidth(),getHeight());
            int x=(getWidth()-fit[0])/2,y=(getHeight()-fit[1])/2;
            canvas.drawBitmap(snapshot,null,new Rect(x,y,x+fit[0],y+fit[1]),paint);canvas.restoreToCount(saved);
        }
    }
}
