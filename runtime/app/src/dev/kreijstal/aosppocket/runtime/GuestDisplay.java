// SPDX-License-Identifier: GPL-3.0-only
package dev.kreijstal.aosppocket.runtime;

import android.content.Context;
import android.graphics.Color;
import android.view.*;
import android.widget.FrameLayout;
import java.util.concurrent.atomic.AtomicLong;

/** A letterboxed, interactive view of the service's fixed-resolution display. */
final class GuestDisplay extends FrameLayout implements SurfaceHolder.Callback {
    private static final AtomicLong NEXT = new AtomicLong();
    private final long id = NEXT.incrementAndGet();
    private final SurfaceView surface;
    private GuestService runtime;
    private int logicalWidth = 1, logicalHeight = 1;
    private boolean attached;
    private int previewHeight = Integer.MAX_VALUE;

    GuestDisplay(Context context) {
        super(context);
        setBackgroundColor(Color.BLACK);
        surface = new SurfaceView(context);
        surface.getHolder().addCallback(this);
        surface.setContentDescription("Android guest display");
        addView(surface);
        surface.setOnTouchListener((view, event) -> {
            if (runtime == null || !runtime.running()) return false;
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
    void limitPreviewHeight(int height) { previewHeight = height; requestLayout(); }
    void setRuntime(GuestService value) {
        if (runtime != value && attached) { Native.detach(id); attached = false; }
        runtime = value;
        if (value != null) {
            if (logicalWidth != value.width || logicalHeight != value.height) {
                logicalWidth = value.width;
                logicalHeight = value.height;
                surface.getHolder().setFixedSize(logicalWidth, logicalHeight);
                requestLayout();
            }
            attach();
        }
    }
    private void attach() {
        SurfaceHolder holder = surface.getHolder();
        if (!attached && runtime != null && runtime.initialized() && holder.getSurface().isValid() &&
                holder.getSurfaceFrame().width() == logicalWidth && holder.getSurfaceFrame().height() == logicalHeight) {
            try { Native.attach(id, holder.getSurface()); attached = true; }
            catch (RuntimeException error) { android.util.Log.e("PocketRuntime", "Display attach failed", error); }
        }
    }
    @Override protected void onMeasure(int widthSpec, int heightSpec) {
        int width = MeasureSpec.getSize(widthSpec), height = MeasureSpec.getSize(heightSpec);
        setMeasuredDimension(width, height);
        int childWidth = width, childHeight = (int)((long)width * logicalHeight / logicalWidth);
        int limit = Math.min(height, previewHeight);
        if (childHeight > limit) { childHeight = limit; childWidth = (int)((long)limit * logicalWidth / logicalHeight); }
        surface.measure(MeasureSpec.makeMeasureSpec(Math.max(1, childWidth), MeasureSpec.EXACTLY),
            MeasureSpec.makeMeasureSpec(Math.max(1, childHeight), MeasureSpec.EXACTLY));
    }
    @Override protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
        int x = (getWidth() - surface.getMeasuredWidth()) / 2;
        int y = (getHeight() - surface.getMeasuredHeight()) / 2;
        surface.layout(x, y, x + surface.getMeasuredWidth(), y + surface.getMeasuredHeight());
    }
    android.graphics.Rect contentBounds() {
        android.graphics.Rect bounds = new android.graphics.Rect();
        surface.getGlobalVisibleRect(bounds);
        return bounds;
    }
    @Override public void surfaceCreated(SurfaceHolder holder) { attach(); }
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) { attach(); }
    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        if (attached) { Native.detach(id); attached = false; }
    }
}
