// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.content.Context;
import android.graphics.Color;
import android.view.*;
import android.widget.FrameLayout;
import android.widget.TextView;
import java.util.concurrent.atomic.AtomicLong;
import dev.kreijstal.aosppocket.bridge.DisplayGeometry;

/** A letterboxed, interactive view of the service's fixed-resolution display. */
final class GuestDisplay extends FrameLayout implements SurfaceHolder.Callback, GuestService.FrameListener {
    private static final AtomicLong NEXT = new AtomicLong();
    private final long id = NEXT.incrementAndGet();
    private final SurfaceView surface;
    private final TextView elsewhere;
    private final boolean dedicated;
    private final boolean fillsBounds;
    private GuestService runtime;
    private int logicalWidth = 1, logicalHeight = 1;
    private boolean attached, owned;
    private Surface directSurface;

    GuestDisplay(Context context, boolean dedicated) {
        this(context,dedicated,false);
    }
    GuestDisplay(Context context, boolean dedicated, boolean fillsBounds) {
        super(context);
        this.dedicated = dedicated;
        this.fillsBounds = fillsBounds;
        setBackgroundColor(Color.BLACK);
        surface = new SurfaceView(context);
        surface.getHolder().addCallback(this);
        surface.setContentDescription("Android guest display");
        surface.setVisibility(INVISIBLE);
        addView(surface);
        elsewhere = new TextView(context);
        elsewhere.setTextColor(Color.WHITE);
        elsewhere.setGravity(Gravity.CENTER);
        elsewhere.setText("Connecting to Android…");
        elsewhere.setOnClickListener(view -> {
            if (runtime != null && !owned) context.startActivity(GuestActivity.intent(context));
        });
        addView(elsewhere);
        surface.setOnTouchListener((view, event) -> {
            if (!owned || runtime == null || !runtime.running()) return false;
            if (runtime.directBackend()) {
                if (event.getActionMasked() == MotionEvent.ACTION_DOWN) getParent().requestDisallowInterceptTouchEvent(true);
                runtime.touch(event, (float)logicalWidth / Math.max(1, surface.getWidth()),
                    (float)logicalHeight / Math.max(1, surface.getHeight()));
                return true;
            }
            int action = event.getActionMasked(), index = event.getActionIndex();
            if (action == MotionEvent.ACTION_MOVE || action == MotionEvent.ACTION_CANCEL) {
                for (int i = 0; i < event.getPointerCount(); i++) send(event, i,
                    action == MotionEvent.ACTION_CANCEL ? 1 : 2);
            } else if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN ||
                       action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP)
                send(event, index, action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN ? 0 : 1);
            return true;
        });
    }
    private void send(MotionEvent event, int index, int action) {
        int x = Math.max(0, Math.min(logicalWidth - 1,
            (int)(event.getX(index) * logicalWidth / Math.max(1, surface.getWidth()))));
        int y = Math.max(0, Math.min(logicalHeight - 1,
            (int)(event.getY(index) * logicalHeight / Math.max(1, surface.getHeight()))));
        Native.touch(action, (int)(id % 1024) * 32 + event.getPointerId(index), x, y);
    }
    void setRuntime(GuestService value) {
        GuestService previous = runtime;
        if(previous!=value)detachDirect();
        runtime = value;
        if (previous != null && previous != value) previous.displayVisible(this, false);
        if (value != null) {
            if (logicalWidth != value.width || logicalHeight != value.height) {
                logicalWidth = value.width;
                logicalHeight = value.height;
                surface.getHolder().setFixedSize(logicalWidth, logicalHeight);
                requestLayout();
            }
            if (previous != value) {
                onDisplayOwnership(false);
                value.displayVisible(this, true);
            }
            attach();
        } else onDisplayOwnership(false);
    }
    @Override public int displayPriority() { return dedicated ? 1 : 0; }
    @Override public void onDisplayOwnership(boolean value) {
        owned = value;
        if (!value)detachDirect();
        if (!value && attached) { Native.detach(id); attached = false; }
        // Hiding the SurfaceView also removes its stale last buffer from composition.
        surface.setVisibility(value ? VISIBLE : INVISIBLE);
        elsewhere.setText(runtime == null ? "Connecting to Android…" :
            "Android is open in its own window.\nTap to return to it.");
        elsewhere.setVisibility(value ? GONE : VISIBLE);
        if (value) attach();
    }
    @Override public void onGuestFrame() { attach(); }
    private void attach() {
        if (!owned) return;
        SurfaceHolder holder = surface.getHolder();
        if (runtime != null && runtime.directBackend()) {
            if (attached) { Native.detach(id); attached = false; }
            if (holder.getSurface().isValid()) {
                directSurface=holder.getSurface();runtime.presentSurface(directSurface);
            }
            return;
        }
        if (!attached && runtime != null && runtime.initialized() && holder.getSurface().isValid() &&
                holder.getSurfaceFrame().width() == logicalWidth && holder.getSurfaceFrame().height() == logicalHeight) {
            try { Native.attach(id, holder.getSurface()); attached = true; }
            catch (RuntimeException error) { android.util.Log.e("PocketRuntime", "Display attach failed", error); }
        }
    }
    @Override protected void onMeasure(int widthSpec, int heightSpec) {
        int width = MeasureSpec.getSize(widthSpec), height = MeasureSpec.getSize(heightSpec);
        setMeasuredDimension(width, height);
        int[] fitted=fillsBounds ? new int[]{width,height} : new DisplayGeometry(logicalWidth,logicalHeight,160).fit(width,height);
        int childWidth=fitted[0],childHeight=fitted[1];
        surface.measure(MeasureSpec.makeMeasureSpec(Math.max(1, childWidth), MeasureSpec.EXACTLY),
            MeasureSpec.makeMeasureSpec(Math.max(1, childHeight), MeasureSpec.EXACTLY));
        elsewhere.measure(MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY),
            MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY));
    }
    @Override protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
        int x = (getWidth() - surface.getMeasuredWidth()) / 2;
        int y = (getHeight() - surface.getMeasuredHeight()) / 2;
        surface.layout(x, y, x + surface.getMeasuredWidth(), y + surface.getMeasuredHeight());
        elsewhere.layout(0, 0, getWidth(), getHeight());
    }
    android.graphics.Rect contentBounds() {
        android.graphics.Rect bounds = new android.graphics.Rect();
        surface.getGlobalVisibleRect(bounds);
        return bounds;
    }
    private void detachDirect() {
        if(directSurface!=null && runtime!=null)runtime.detachSurface(directSurface);
        directSurface=null;
    }
    @Override public void surfaceCreated(SurfaceHolder holder) { attach(); }
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) { attach(); }
    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        detachDirect();
        // Window lifecycle, not SurfaceView visibility, controls ownership. A losing
        // window hides its surface without withdrawing its request for later handback.
        if (attached) { Native.detach(id); attached = false; }
    }
}
