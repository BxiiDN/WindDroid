package com.bxiidn.winddroid;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;

/**
 * Shows the guest screen scaled to fit and turns touches into absolute pointer
 * events for the USB tablet: one finger moves and clicks (left button), a second
 * finger held down turns the touch into a right click, two-finger drag scrolls.
 */
public class FramebufferView extends View {
    public interface PointerSink {
        void pointer(int x, int y, int buttons, int wheel);
    }

    private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
    private final Rect src = new Rect(), dst = new Rect();
    private volatile Bitmap frame;
    private PointerSink sink;
    private int lastX, lastY, buttons;
    private float scrollY0;
    private boolean twoFinger;

    public FramebufferView(Context c, AttributeSet a) {
        super(c, a);
        setFocusable(true);
        setFocusableInTouchMode(true);
    }

    public void setSink(PointerSink s) {
        sink = s;
    }

    public void setFrame(Bitmap b) {
        frame = b;
        postInvalidateOnAnimation();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        canvas.drawColor(Color.BLACK);
        Bitmap b = frame;
        if (b == null) return;
        fit(b);
        synchronized (b) {
            canvas.drawBitmap(b, src, dst, paint);
        }
    }

    private void fit(Bitmap b) {
        int vw = getWidth(), vh = getHeight(), bw = b.getWidth(), bh = b.getHeight();
        src.set(0, 0, bw, bh);
        float s = Math.min(vw / (float) bw, vh / (float) bh);
        int w = Math.round(bw * s), h = Math.round(bh * s);
        int x = (vw - w) / 2, y = (vh - h) / 2;
        dst.set(x, y, x + w, y + h);
    }

    private int gx(float x) {
        if (dst.width() <= 0) return 0;
        return Math.max(0, Math.min(32767, Math.round((x - dst.left) * 32767f / dst.width())));
    }

    private int gy(float y) {
        if (dst.height() <= 0) return 0;
        return Math.max(0, Math.min(32767, Math.round((y - dst.top) * 32767f / dst.height())));
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (sink == null) return false;
        requestFocus();
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                twoFinger = false;
                lastX = gx(e.getX());
                lastY = gy(e.getY());
                buttons = 1;
                sink.pointer(lastX, lastY, 0, 0);       /* move first, then press */
                sink.pointer(lastX, lastY, buttons, 0);
                return true;
            case MotionEvent.ACTION_POINTER_DOWN:
                if (e.getPointerCount() == 2) {
                    twoFinger = true;
                    scrollY0 = (e.getY(0) + e.getY(1)) / 2;
                    sink.pointer(lastX, lastY, 0, 0);   /* cancel the left press */
                }
                return true;
            case MotionEvent.ACTION_MOVE:
                if (twoFinger && e.getPointerCount() >= 2) {
                    float y = (e.getY(0) + e.getY(1)) / 2;
                    float d = (scrollY0 - y) / (getResources().getDisplayMetrics().density * 24);
                    if (Math.abs(d) >= 1) {
                        sink.pointer(lastX, lastY, 0, (int) -d);
                        scrollY0 = y;
                    }
                    return true;
                }
                if (!twoFinger) {
                    lastX = gx(e.getX());
                    lastY = gy(e.getY());
                    sink.pointer(lastX, lastY, buttons, 0);
                }
                return true;
            case MotionEvent.ACTION_POINTER_UP:
                return true;
            case MotionEvent.ACTION_UP:
                if (twoFinger) {
                    if (e.getEventTime() - e.getDownTime() < 350) {    /* two-finger tap: right click */
                        sink.pointer(lastX, lastY, 2, 0);
                        sink.pointer(lastX, lastY, 0, 0);
                    }
                } else {
                    sink.pointer(lastX, lastY, 0, 0);
                }
                buttons = 0;
                twoFinger = false;
                return true;
            case MotionEvent.ACTION_CANCEL:
                sink.pointer(lastX, lastY, 0, 0);
                buttons = 0;
                return true;
            default:
                return true;
        }
    }
}
