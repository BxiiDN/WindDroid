package com.bxiidn.winddroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.text.Editable;
import android.text.TextWatcher;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The UEFI / Windows virtual machine: EDK2 firmware in flash, an NVMe system disk,
 * an optional USB DVD drive with an installer ISO, USB keyboard and tablet, and the
 * ramfb framebuffer shown full screen.
 */
public class VmActivity extends Activity implements Engine.Listener {
    static final String TAG = "WindDroid";
    static final String PREF_DISK = "disk_uri", PREF_ISO = "iso_uri", PREF_RAM = "vm_ram_mb", PREF_CPUS = "vm_cpus";
    static final String PREF_DISK_NAME = "disk_name", PREF_ISO_NAME = "iso_name";

    private final Engine engine = new Engine();
    private final Handler ui = new Handler(Looper.getMainLooper());
    private FramebufferView screen;
    private TextView console, status;
    private ScrollView consoleScroll;
    private EditText ime;
    private final StringBuilder serial = new StringBuilder();
    private final StringBuilder logLine = new StringBuilder();
    private boolean logOutput;
    private String typeAfter, typeText;
    private final StringBuilder watch = new StringBuilder();
    private int frameW, frameH;
    private boolean ctrlSticky, altSticky, shiftSticky, guiSticky;
    private Button ctrlBtn, altBtn, shiftBtn, guiBtn;
    private boolean suppressWatcher;
    private boolean stopping;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        setContentView(R.layout.activity_vm);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        screen = findViewById(R.id.screen);
        console = findViewById(R.id.console);
        consoleScroll = findViewById(R.id.console_scroll);
        status = findViewById(R.id.vm_status);
        ime = findViewById(R.id.ime);
        screen.setSink(engine::pointer);
        buildKeyBar(findViewById(R.id.keybar));
        setupIme();

        Intent it = getIntent();
        logOutput = it.getBooleanExtra("autorun", false);
        typeAfter = it.getStringExtra("type_after");
        typeText = it.getStringExtra("type");
        boolean noDisks = it.getBooleanExtra("no_disks", false);
        try {
            startVm(noDisks);
        } catch (RuntimeException | IOException e) {
            Log.e(TAG, "VM start failed", e);
            if (logOutput) Log.i(TAG, "STOPPED status=-2 start failed: " + e.getMessage());
            new AlertDialog.Builder(this)
                    .setTitle(R.string.error_title)
                    .setMessage(String.valueOf(e.getMessage()))
                    .setPositiveButton(android.R.string.ok, (d, w) -> finish())
                    .setCancelable(false)
                    .show();
        }
    }

    // ------------------------------------------------------------ machine

    static byte[] asset(Context c, String name) throws IOException {
        try (InputStream in = c.getAssets().open(name)) {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            return out.toByteArray();
        }
    }

    private File varsFile() {
        return new File(getFilesDir(), "uefi_vars.fd");
    }

    private void startVm(boolean noDisks) throws IOException {
        SharedPreferences prefs = getSharedPreferences("winddroid", MODE_PRIVATE);
        long ram = prefs.getLong(PREF_RAM, 2048);
        int cpus = prefs.getInt(PREF_CPUS, 2);
        byte[] code = asset(this, "QEMU_EFI.fd");
        byte[] vars = null;
        File vf = varsFile();
        if (vf.length() > 0) {
            vars = java.nio.file.Files.readAllBytes(vf.toPath());
        } else {
            vars = asset(this, "QEMU_VARS.fd");
        }
        final byte[] fvars = vars;

        // open the disk images now (UI thread, ContentResolver); the engine owns the fds afterwards
        List<int[]> fds = new ArrayList<>();
        List<String> names = new ArrayList<>();
        StringBuilder desc = new StringBuilder();
        if (!noDisks) {
            String disk = prefs.getString(PREF_DISK, null);
            String iso = prefs.getString(PREF_ISO, null);
            if (disk != null) {
                int fd = openFd(Uri.parse(disk), "rw");
                fds.add(new int[] {fd, Engine.DISK_NVME});
                names.add(prefs.getString(PREF_DISK_NAME, "disk"));
                desc.append("NVMe: ").append(prefs.getString(PREF_DISK_NAME, "disk"));
            }
            if (iso != null) {
                int fd = openFd(Uri.parse(iso), "r");
                fds.add(new int[] {fd, Engine.DISK_CDROM});
                names.add(prefs.getString(PREF_ISO_NAME, "iso"));
                if (desc.length() > 0) desc.append("  ·  ");
                desc.append("DVD: ").append(prefs.getString(PREF_ISO_NAME, "iso"));
            }
        }
        final String info0 = String.format(Locale.US, "RAM %d MB  ·  %d CPU  ·  %s", ram, cpus, desc.length() > 0 ? desc : "tanpa disk");
        status.setText(info0);
        try {
            engine.start(ram, cpus, true, h -> {
                Engine.loadFirmware(h, code, fvars);
                int i = 0;
                for (int[] f : fds) {
                    int fd = f[0];
                    f[0] = -1;                      /* ownership passes to the engine */
                    Engine.attachDisk(h, fd, f[1], false, names.get(i++));
                }
                Engine.attachUsbInput(h);
                jit = Engine.jitActive(h);
            }, this);
        } finally {
            for (int[] f : fds) if (f[0] >= 0) closeFd(f[0]);
        }
        status.setText(info0 + (jit ? "  ·  JIT" : "  ·  interpreter"));
        if (logOutput) Log.i(TAG, jit ? "JIT active" : "JIT not active (interpreter)");
    }

    private boolean jit;

    private int openFd(Uri uri, String mode) throws IOException {
        ParcelFileDescriptor pfd;
        try {
            pfd = getContentResolver().openFileDescriptor(uri, mode);
        } catch (SecurityException e) {
            throw new IOException(getString(R.string.err_permission, uri.getLastPathSegment()));
        }
        if (pfd == null) throw new IOException("tidak bisa membuka " + uri);
        return pfd.detachFd();
    }

    private static void closeFd(int fd) {
        try {
            ParcelFileDescriptor.adoptFd(fd).close();
        } catch (IOException ignored) {
        }
    }

    // ------------------------------------------------------------ Engine.Listener (VM thread)

    @Override
    public void onOutput(byte[] data) {
        String s = new String(data, StandardCharsets.ISO_8859_1);
        if (logOutput) {
            for (int i = 0; i < s.length(); i++) {
                char c = s.charAt(i);
                // menus are drawn with cursor positioning: a cursor move or CR also ends a log line
                if (c == '\n' || ((c == 27 || c == '\r') && logLine.length() > 0) || logLine.length() > 300) {
                    String line = logLine.toString().replaceAll("\u001b\\[[0-9;=?]*[A-Za-z]", " ").trim();
                    if (!line.isEmpty()) Log.i(TAG, line);
                    logLine.setLength(0);
                }
                if (c != '\n' && c != '\r') logLine.append(c);
            }
        }
        if (typeAfter != null) {
            watch.append(s.replaceAll("\u001b\\[[0-9;=?]*[A-Za-z]", " "));
            if (watch.indexOf(typeAfter) >= 0) {
                typeAfter = null;
                if (logOutput) Log.i(TAG, "TYPING on the USB keyboard");
                typeString(typeText == null ? "\r" : typeText.replace("\\r", "\r"));
            } else if (watch.length() > 4096) {
                watch.delete(0, watch.length() - 256);
            }
        }
        synchronized (serial) {
            serial.append(s.replaceAll("\u001b\\[[0-9;=?]*[A-Za-z]", ""));
            if (serial.length() > 60000) serial.delete(0, serial.length() - 50000);
        }
        if (console.getVisibility() == View.VISIBLE) ui.post(this::renderConsole);
    }

    @Override
    public void onFrame(Bitmap frame) {
        screen.setFrame(frame);
        if (frame.getWidth() != frameW || frame.getHeight() != frameH) {
            frameW = frame.getWidth();
            frameH = frame.getHeight();
            if (logOutput) Log.i(TAG, "FB " + frameW + "x" + frameH);
        }
    }

    @Override
    public void onVars(byte[] vars) {
        File f = varsFile(), tmp = new File(getFilesDir(), "uefi_vars.tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(vars);
            out.getFD().sync();
        } catch (IOException e) {
            Log.w(TAG, "saving UEFI variables failed", e);
            return;
        }
        if (!tmp.renameTo(f)) Log.w(TAG, "saving UEFI variables failed (rename)");
    }

    @Override
    public void onStats(long instructions, double mips) {
        ui.post(() -> {
            if (engine.isRunning()) {
                String base = status.getTag() instanceof String ? (String) status.getTag() : status.getText().toString();
                status.setTag(base);
                status.setText(String.format(Locale.US, "%s  ·  %.0f MIPS", base, mips));
            }
        });
    }

    @Override
    public void onStopped(int st, int exitCode, String detail) {
        if (logOutput) Log.i(TAG, "STOPPED status=" + st + " exit=" + exitCode + " " + detail);
        ui.post(() -> {
            if (stopping || isFinishing()) return;
            String msg;
            switch (st) {
                case Engine.RUN_SHUTDOWN: msg = getString(R.string.vm_off); break;
                case Engine.RUN_RESET: msg = getString(R.string.vm_reset); break;
                case Engine.RUN_ERROR: msg = getString(R.string.stopped_error) + "\n\n" + detail; break;
                default: msg = getString(R.string.stopped_user); break;
            }
            new AlertDialog.Builder(this)
                    .setMessage(msg)
                    .setPositiveButton(android.R.string.ok, (d, w) -> finish())
                    .setCancelable(false)
                    .show();
        });
    }

    // ------------------------------------------------------------ keyboard

    private void tap(int usage) {
        engine.key(usage, true);
        engine.key(usage, false);
    }

    private void typeString(String s) {
        for (int i = 0; i < s.length(); i++) typeChar(s.charAt(i));
    }

    private void typeChar(int ch) {
        int u = Engine.asciiToHid(ch);
        if ((u & 0xFF) == 0) return;
        boolean shift = (u & 0x100) != 0;
        if (shift) engine.key(KeyMap.SHIFT, true);
        tap(u & 0xFF);
        if (shift) engine.key(KeyMap.SHIFT, false);
        releaseSticky();
    }

    private void setupIme() {
        resetIme();
        ime.addTextChangedListener(new TextWatcher() {
            private int before;

            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
                before = s.length();
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {}

            @Override
            public void afterTextChanged(Editable s) {
                if (suppressWatcher) return;
                int now = s.length();
                if (now > before) {
                    CharSequence added = s.subSequence(before, now);
                    for (int i = 0; i < added.length(); i++) typeChar(added.charAt(i));
                } else if (now < before) {
                    for (int i = now; i < before; i++) tap(0x2A);
                }
                if (now < 1 || now > 64) resetIme();
            }
        });
        ime.setOnEditorActionListener((v, actionId, ev) -> {
            if (actionId == EditorInfo.IME_ACTION_DONE || actionId == EditorInfo.IME_ACTION_SEND
                    || actionId == EditorInfo.IME_NULL) {
                tap(KeyMap.ENTER);
                releaseSticky();
                return true;
            }
            return false;
        });
    }

    private void resetIme() {
        suppressWatcher = true;
        ime.setText(">");
        ime.setSelection(1);
        suppressWatcher = false;
    }

    private void toggleIme() {
        InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
        ime.requestFocus();
        imm.toggleSoftInput(InputMethodManager.SHOW_FORCED, 0);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int code = e.getKeyCode();
        if (code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_VOLUME_UP || code == KeyEvent.KEYCODE_VOLUME_DOWN)
            return super.dispatchKeyEvent(e);
        boolean hardware = e.getDeviceId() != KeyCharacterMap.VIRTUAL_KEYBOARD
                && (e.getSource() & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD
                && e.getDevice() != null && !e.getDevice().isVirtual();
        int usage = KeyMap.usage(code);
        // physical keyboards send scan codes as they are; the soft keyboard only for keys without text
        if (usage != 0 && (hardware || code == KeyEvent.KEYCODE_DEL || code == KeyEvent.KEYCODE_ENTER
                || code == KeyEvent.KEYCODE_FORWARD_DEL || (code >= KeyEvent.KEYCODE_DPAD_UP && code <= KeyEvent.KEYCODE_DPAD_RIGHT))) {
            if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0) {
                engine.key(usage, true);
            } else if (e.getAction() == KeyEvent.ACTION_DOWN) {
                engine.key(usage, false);              /* auto-repeat */
                engine.key(usage, true);
            } else if (e.getAction() == KeyEvent.ACTION_UP) {
                engine.key(usage, false);
                if (usage < 0xE0) releaseSticky();
            }
            return true;
        }
        return super.dispatchKeyEvent(e);
    }

    private Button bar(LinearLayout row, String label, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setMinWidth(0);
        b.setMinimumWidth(0);
        b.setPadding(24, 0, 24, 0);
        b.setOnClickListener(l);
        row.addView(b);
        return b;
    }

    private void buildKeyBar(LinearLayout row) {
        bar(row, "⌨", v -> toggleIme());
        bar(row, "Esc", v -> { tap(KeyMap.ESC); releaseSticky(); });
        bar(row, "Tab", v -> { tap(KeyMap.TAB); releaseSticky(); });
        ctrlBtn = bar(row, "Ctrl", v -> { ctrlSticky = !ctrlSticky; engine.key(KeyMap.CTRL, ctrlSticky); markSticky(); });
        altBtn = bar(row, "Alt", v -> { altSticky = !altSticky; engine.key(KeyMap.ALT, altSticky); markSticky(); });
        shiftBtn = bar(row, "Shift", v -> { shiftSticky = !shiftSticky; engine.key(KeyMap.SHIFT, shiftSticky); markSticky(); });
        guiBtn = bar(row, "⊞ Win", v -> {
            if (!guiSticky && !ctrlSticky && !altSticky && !shiftSticky) { tap(KeyMap.GUI); return; }
            guiSticky = !guiSticky;
            engine.key(KeyMap.GUI, guiSticky);
            markSticky();
        });
        bar(row, "Del", v -> { tap(KeyMap.DELETE); releaseSticky(); });
        bar(row, "←", v -> tap(KeyMap.LEFT));
        bar(row, "↑", v -> tap(KeyMap.UP));
        bar(row, "↓", v -> tap(KeyMap.DOWN));
        bar(row, "→", v -> tap(KeyMap.RIGHT));
        bar(row, "Enter", v -> { tap(KeyMap.ENTER); releaseSticky(); });
        bar(row, "F2", v -> tap(KeyMap.F2));
        bar(row, "F12", v -> tap(KeyMap.F12));
        bar(row, "Ctrl+Alt+Del", v -> {
            engine.key(KeyMap.CTRL, true);
            engine.key(KeyMap.ALT, true);
            tap(KeyMap.DELETE);
            engine.key(KeyMap.ALT, false);
            engine.key(KeyMap.CTRL, false);
        });
        bar(row, "Shift+F10", v -> {
            engine.key(KeyMap.SHIFT, true);
            tap(0x43);
            engine.key(KeyMap.SHIFT, false);
        });
        bar(row, getString(R.string.btn_console), v -> {
            boolean show = consoleScroll.getVisibility() != View.VISIBLE;
            consoleScroll.setVisibility(show ? View.VISIBLE : View.GONE);
            console.setVisibility(show ? View.VISIBLE : View.GONE);
            if (show) renderConsole();
        });
        bar(row, "■", v -> confirmStop());
    }

    private void markSticky() {
        int on = 0xFF4FC3F7, off = 0;
        ctrlBtn.setBackgroundColor(ctrlSticky ? on : off);
        altBtn.setBackgroundColor(altSticky ? on : off);
        shiftBtn.setBackgroundColor(shiftSticky ? on : off);
        guiBtn.setBackgroundColor(guiSticky ? on : off);
    }

    private void releaseSticky() {
        boolean any = ctrlSticky || altSticky || shiftSticky || guiSticky;
        if (!any) return;
        if (ctrlSticky) engine.key(KeyMap.CTRL, false);
        if (altSticky) engine.key(KeyMap.ALT, false);
        if (shiftSticky) engine.key(KeyMap.SHIFT, false);
        if (guiSticky) engine.key(KeyMap.GUI, false);
        ctrlSticky = altSticky = shiftSticky = guiSticky = false;
        ui.post(this::markSticky);
    }

    private void renderConsole() {
        String t;
        synchronized (serial) {
            t = serial.toString();
        }
        console.setText(t);
        consoleScroll.post(() -> consoleScroll.fullScroll(View.FOCUS_DOWN));
    }

    // ------------------------------------------------------------ lifecycle

    private void confirmStop() {
        new AlertDialog.Builder(this)
                .setMessage(R.string.vm_confirm_stop)
                .setPositiveButton(R.string.btn_stop, (d, w) -> {
                    stopping = true;
                    engine.stop();
                    finish();
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    @Override
    public void onBackPressed() {
        if (engine.isRunning()) confirmStop();
        else super.onBackPressed();
    }

    @Override
    protected void onDestroy() {
        stopping = true;
        engine.stop();
        super.onDestroy();
    }
}
