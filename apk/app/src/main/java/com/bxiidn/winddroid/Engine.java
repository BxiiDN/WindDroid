package com.bxiidn.winddroid;

import java.util.concurrent.ConcurrentLinkedQueue;

/**
 * Runs one WindDroid virtual machine on a dedicated thread. All native calls for a
 * machine happen on that thread; other threads talk to it through queues.
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

    public interface Listener {
        void onOutput(byte[] data);

        void onStats(long instructions, double mips);

        void onStopped(int status, int exitCode, String detail);
    }

    private final ConcurrentLinkedQueue<byte[]> input = new ConcurrentLinkedQueue<>();
    private final Object lock = new Object();
    private volatile boolean running;
    private Thread thread;

    public static String version() {
        return nativeVersion();
    }

    public boolean isRunning() {
        return running;
    }

    /** Creates a machine, loads the image and starts it. Throws if the image cannot be loaded. */
    public synchronized void start(byte[] image, long ramMb, Listener listener) {
        stop();
        long h = nativeCreate(ramMb, true);
        if (h == 0) throw new IllegalStateException("Tidak cukup memori untuk RAM " + ramMb + " MB");
        if (nativeLoad(h, image) != 0) {
            String err = nativeError(h);
            nativeDestroy(h);
            throw new IllegalArgumentException(err);
        }
        input.clear();
        running = true;
        thread = new Thread(() -> loop(h, listener), "winddroid-vm");
        thread.setPriority(Thread.MAX_PRIORITY);
        thread.start();
    }

    public void send(byte[] data) {
        input.add(data);
        synchronized (lock) {
            lock.notifyAll();
        }
    }

    public synchronized void stop() {
        running = false;
        synchronized (lock) {
            lock.notifyAll();
        }
        if (thread != null) {
            try {
                thread.join(5000);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            thread = null;
        }
    }

    private void loop(long h, Listener l) {
        int status = STOPPED_BY_USER;
        String detail = "";
        long statT = System.nanoTime(), statN = 0;
        try {
            while (running) {
                byte[] in;
                while ((in = input.poll()) != null) nativeInput(h, in);

                int r = nativeRun(h, 4_000_000L);
                byte[] out = nativeTakeOutput(h);
                if (out != null) l.onOutput(out);

                long now = System.nanoTime();
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
                            long waitNs = ns < 0 ? 250_000_000L : Math.min(ns, 250_000_000L);
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
            int exit = nativeExitCode(h);
            l.onStats(nativeIcount(h), 0);
            nativeDestroy(h);
            running = false;
            l.onStopped(status, exit, detail);
        }
    }

    private static native long nativeCreate(long ramMb, boolean realtime);

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
}
