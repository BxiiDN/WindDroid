package com.bxiidn.winddroid;

import android.graphics.Bitmap;

import java.util.concurrent.ConcurrentLinkedQueue;

/**
 * Runs one WindDroid virtual machine on a dedicated thread. All native calls for a
 * machine happen on that thread (setup happens before it starts); other threads talk
 * to it through queues.
 */
public final class Engine {
    static {
        System.loadLibrary("winddroid");
    }

    public static final int RUN_BUDGET = 0;
    public static final int RUN_IDLE = 1;
    public static final int RUN_SHUTDOWN = 2;
    public static final int RUN_RESET = 3;
    public static final int RUN_ERROR = 4;
    public static final int STOPPED_BY_USER = -1;

    public static final int DISK_NVME = 0;
    public static final int DISK_USB = 1;
    public static final int DISK_CDROM = 2;

    public static final int IMG_RAW = 0, IMG_ISO = 1, IMG_VHD = 2, IMG_VHDX = 3;

    public interface Listener {
        void onOutput(byte[] data);

        void onStats(long instructions, double mips);

        void onStopped(int status, int exitCode, String detail);

        /** A new frame was copied into the bitmap (VM thread); the bitmap is locked while copying. */
        default void onFrame(Bitmap frame) {}

        /** The firmware variable store changed (VM thread). */
        default void onVars(byte[] vars) {}
    }

    /** Prepares a freshly created machine; throws with a readable message on failure. */
    public interface Setup {
        void apply(long handle);
    }

    private static final class Ev {
        final int kind, a, b, c, d;
        final byte[] data;

        Ev(int kind, int a, int b, int c, int d, byte[] data) {
            this.kind = kind; this.a = a; this.b = b; this.c = c; this.d = d; this.data = data;
        }
    }

    private static final int EV_SERIAL = 0, EV_KEY = 1, EV_POINTER = 2;

    private final ConcurrentLinkedQueue<Ev> input = new ConcurrentLinkedQueue<>();
    private final Object lock = new Object();
    private volatile boolean running;
    private Thread thread;
    private volatile Bitmap frame;
    private volatile boolean display;

    public static String version() {
        return nativeVersion();
    }

    public boolean isRunning() {
        return running;
    }

    /** Creates a machine, loads the image and starts it. Throws if the image cannot be loaded. */
    public synchronized void start(byte[] image, long ramMb, Listener listener) {
        start(ramMb, 1, false, h -> {
            if (nativeLoad(h, image) != 0) throw new IllegalArgumentException(nativeError(h));
        }, listener);
    }

    /** Creates a machine, runs the setup and starts it on the VM thread. */
    public synchronized void start(long ramMb, int cpus, boolean withDisplay, Setup setup, Listener listener) {
        stop();
        long h = nativeCreate(ramMb, cpus, true);
        if (h == 0) throw new IllegalStateException("Tidak cukup memori untuk RAM " + ramMb + " MB");
        try {
            setup.apply(h);
        } catch (RuntimeException e) {
            nativeDestroy(h);
            throw e;
        }
        input.clear();
        display = withDisplay;
        frame = null;
        running = true;
        thread = new Thread(() -> loop(h, listener), "winddroid-vm");
        thread.setPriority(Thread.MAX_PRIORITY);
        thread.start();
    }

    private void post(Ev e) {
        input.add(e);
        synchronized (lock) {
            lock.notifyAll();
        }
    }

    public void send(byte[] data) {
        post(new Ev(EV_SERIAL, 0, 0, 0, 0, data));
    }

    /** USB keyboard: HID usage (page 7), pressed or released. */
    public void key(int usage, boolean down) {
        post(new Ev(EV_KEY, usage, down ? 1 : 0, 0, 0, null));
    }

    /** USB tablet: absolute position 0..32767, buttons bit0 left / bit1 right / bit2 middle. */
    public void pointer(int x, int y, int buttons, int wheel) {
        post(new Ev(EV_POINTER, x, y, buttons, wheel, null));
    }

    public Bitmap frame() {
        return frame;
    }

    public synchronized void stop() {
        running = false;
        synchronized (lock) {
            lock.notifyAll();
        }
        if (thread != null) {
            try {
                thread.join(8000);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            thread = null;
        }
    }

    private void loop(long h, Listener l) {
        int status = STOPPED_BY_USER;
        String detail = "";
        long statT = System.nanoTime(), statN = 0, frameT = 0, varsT = System.nanoTime();
        int[] info = new int[3];
        int gen = -1;
        try {
            while (running) {
                long slice = 4_000_000L;
                Ev e;
                boolean keySent = false;
                while ((e = input.peek()) != null) {
                    if (e.kind == EV_KEY) {
                        // one key change per slice, so the guest polls every HID report
                        if (keySent) { slice = 400_000L; break; }
                        nativeKey(h, e.a, e.b != 0);
                        keySent = true;
                        slice = 400_000L;
                    } else if (e.kind == EV_POINTER) {
                        nativePointer(h, e.a, e.b, e.c, e.d);
                    } else {
                        nativeInput(h, e.data);
                    }
                    input.poll();
                }

                int r = nativeRun(h, slice);
                byte[] out = nativeTakeOutput(h);
                if (out != null) l.onOutput(out);

                long now = System.nanoTime();
                if (display && now - frameT > 33_000_000L) {
                    frameT = now;
                    if (nativeFbInfo(h, info) != 0) {
                        Bitmap b = frame;
                        if (b == null || info[2] != gen || b.getWidth() != info[0] || b.getHeight() != info[1]) {
                            b = Bitmap.createBitmap(info[0], info[1], Bitmap.Config.ARGB_8888);
                            gen = info[2];
                        }
                        synchronized (b) {
                            nativeBlit(h, b);
                        }
                        frame = b;
                        l.onFrame(b);
                    }
                }
                if (now - varsT > 3_000_000_000L) {
                    varsT = now;
                    byte[] v = nativeTakeVars(h, false);
                    if (v != null) l.onVars(v);
                }
                if (now - statT > 500_000_000L) {
                    long n = nativeIcount(h);
                    l.onStats(n, (n - statN) / ((now - statT) / 1000.0));
                    statT = now;
                    statN = n;
                }

                if (r == RUN_BUDGET) continue;
                if (r == RUN_IDLE) {
                    long ns = nativeIdleNs(h);
                    synchronized (lock) {
                        if (running && input.isEmpty()) {
                            long cap = display ? 16_000_000L : 250_000_000L;
                            long waitNs = ns < 0 ? cap : Math.min(ns, cap);
                            if (waitNs > 0) lock.wait(waitNs / 1_000_000L, (int) (waitNs % 1_000_000L));
                        }
                    }
                    continue;
                }
                status = r;
                if (r == RUN_ERROR) detail = nativeError(h) + "\n" + nativeState(h);
                break;
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } finally {
            byte[] out = nativeTakeOutput(h);
            if (out != null) l.onOutput(out);
            byte[] v = nativeTakeVars(h, false);
            if (v != null) l.onVars(v);
            int exit = nativeExitCode(h);
            l.onStats(nativeIcount(h), 0);
            nativeDestroy(h);
            running = false;
            l.onStopped(status, exit, detail);
        }
    }

    // setup helpers (call only from Setup.apply)

    static void loadFirmware(long h, byte[] code, byte[] vars) {
        String err = nativeLoadFirmware(h, code, vars);
        if (err != null) throw new IllegalArgumentException("Firmware: " + err);
    }

    /** Attaches a disk image; the engine takes ownership of fd. */
    static void attachDisk(long h, int fd, int kind, boolean readOnly, String name) {
        String err = nativeAttachDisk(h, fd, kind, readOnly);
        if (err != null) throw new IllegalArgumentException(name + ": " + err);
    }

    static void attachUsbInput(long h) {
        if (nativeAttachUsbInput(h) != 0) throw new IllegalStateException("USB: " + nativeError(h));
    }

    /** USB network adapter (CDC NCM) behind the app's NAT: 10.0.2.15 in the guest. */
    static void attachUsbNet(long h) {
        if (nativeAttachUsbNet(h) != 0) throw new IllegalStateException("USB network: " + nativeError(h));
    }

    public static int imageFormat(int fd) {
        return nativeImageFormat(fd);
    }

    /** Writes an empty dynamic VHDX into fd (closes it). */
    public static boolean createVhdx(int fd, long sizeBytes) {
        return nativeCreateVhdx(fd, sizeBytes) == 0;
    }

    /** HID usage for an ASCII character (bit 8: needs Shift), 0 if none. */
    public static int asciiToHid(int ch) {
        return nativeAsciiToHid(ch);
    }

    private static native long nativeCreate(long ramMb, int cpus, boolean realtime);

    private static native void nativeDestroy(long h);

    private static native int nativeLoad(long h, byte[] image);

    private static native int nativeRun(long h, long maxInsns);

    private static native long nativeIdleNs(long h);

    private static native void nativeInput(long h, byte[] data);

    private static native byte[] nativeTakeOutput(long h);

    private static native long nativeIcount(long h);

    private static native int nativeExitCode(long h);

    private static native String nativeError(long h);

    private static native String nativeState(long h);

    private static native String nativeVersion();

    private static native String nativeLoadFirmware(long h, byte[] code, byte[] vars);

    private static native String nativeAttachDisk(long h, int fd, int kind, boolean readOnly);

    private static native int nativeImageFormat(int fd);

    private static native int nativeCreateVhdx(int fd, long size);

    private static native int nativeAttachUsbInput(long h);

    private static native int nativeAttachUsbNet(long h);

    private static native void nativeKey(long h, int usage, boolean down);

    private static native void nativePointer(long h, int x, int y, int buttons, int wheel);

    private static native int nativeAsciiToHid(int ch);

    private static native int nativeFbInfo(long h, int[] out);

    private static native int nativeBlit(long h, Bitmap bitmap);

    private static native byte[] nativeTakeVars(long h, boolean force);

    private static native boolean nativeJitActive(long h);

    /** True when guest code is translated to host code (ARM64 devices); call from Setup. */
    static boolean jitActive(long h) {
        return nativeJitActive(h);
    }
}
