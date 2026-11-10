package com.bxiidn.winddroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.database.Cursor;
import android.provider.OpenableColumns;
import android.text.method.ScrollingMovementMethod;
import android.util.Log;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

public class MainActivity extends Activity implements Engine.Listener {
    private static final String TAG = "WindDroid";
    private static final int REQ_IMAGE = 1, REQ_DISK = 2, REQ_ISO = 3, REQ_NEWDISK = 4;
    private static final long[] VM_RAM_CHOICES = {1024, 2048, 3072, 4096, 6144, 8192};
    private static final int[] NEWDISK_GB = {32, 64, 128, 256};
    private long pendingNewDiskGb;
    private TextView vmDisks;
    private Button vmRamButton, vmCpuButton, vmNetButton, vmJitButton;
    private static final int[] VM_CPU_CHOICES = {1, 2, 4};
    private static final long MAX_IMAGE = 512L << 20;
    private static final int MAX_TERMINAL_CHARS = 120_000;
    private static final long[] RAM_CHOICES = {128, 256, 512, 1024, 2048};

    private final Engine engine = new Engine();
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final StringBuilder term = new StringBuilder();
    private final TerminalParser parser = new TerminalParser();

    private TextView terminal, status;
    private ScrollView scroll;
    private EditText input;
    private Button ramButton;
    private SharedPreferences prefs;
    private boolean renderPosted;
    private String imageName = "";
    private volatile boolean logOutput;
    private final StringBuilder logLine = new StringBuilder();

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        setContentView(R.layout.activity_main);
        prefs = getSharedPreferences("winddroid", MODE_PRIVATE);

        terminal = findViewById(R.id.terminal);
        status = findViewById(R.id.status);
        scroll = findViewById(R.id.scroll);
        input = findViewById(R.id.input);
        ramButton = findViewById(R.id.btn_ram);
        terminal.setMovementMethod(new ScrollingMovementMethod());

        findViewById(R.id.btn_demo).setOnClickListener(v -> startDemo());
        findViewById(R.id.btn_load).setOnClickListener(v -> pickImage());
        findViewById(R.id.btn_stop).setOnClickListener(v -> engine.stop());
        findViewById(R.id.btn_clear).setOnClickListener(v -> {
            term.setLength(0);
            parser.reset();
            render();
        });
        findViewById(R.id.btn_send).setOnClickListener(v -> sendLine());
        ramButton.setOnClickListener(v -> cycleRam());
        vmDisks = findViewById(R.id.vm_disks);
        vmRamButton = findViewById(R.id.btn_vm_ram);
        vmCpuButton = findViewById(R.id.btn_vm_cpu);
        vmNetButton = findViewById(R.id.btn_vm_net);
        vmJitButton = findViewById(R.id.btn_vm_jit);
        vmJitButton.setOnClickListener(v -> {
            prefs.edit().putBoolean(VmActivity.PREF_JIT, !prefs.getBoolean(VmActivity.PREF_JIT, true)).apply();
            updateVmLabels();
        });
        vmNetButton.setOnClickListener(v -> {
            prefs.edit().putBoolean(VmActivity.PREF_NET, !prefs.getBoolean(VmActivity.PREF_NET, true)).apply();
            updateVmLabels();
        });
        vmCpuButton.setOnClickListener(v -> {
            int cur = prefs.getInt(VmActivity.PREF_CPUS, 2), next = VM_CPU_CHOICES[0];
            for (int i = 0; i < VM_CPU_CHOICES.length; i++)
                if (VM_CPU_CHOICES[i] == cur) next = VM_CPU_CHOICES[(i + 1) % VM_CPU_CHOICES.length];
            prefs.edit().putInt(VmActivity.PREF_CPUS, next).apply();
            updateVmLabels();
        });
        findViewById(R.id.btn_vm).setOnClickListener(v -> startVm());
        findViewById(R.id.btn_disk).setOnClickListener(v -> pickDocument(REQ_DISK));
        findViewById(R.id.btn_iso).setOnClickListener(v -> pickDocument(REQ_ISO));
        findViewById(R.id.btn_disk).setOnLongClickListener(v -> { forget(VmActivity.PREF_DISK, VmActivity.PREF_DISK_NAME); return true; });
        findViewById(R.id.btn_iso).setOnLongClickListener(v -> { forget(VmActivity.PREF_ISO, VmActivity.PREF_ISO_NAME); return true; });
        findViewById(R.id.btn_newdisk).setOnClickListener(v -> newDisk());
        findViewById(R.id.btn_reset_vars).setOnClickListener(v -> {
            new java.io.File(getFilesDir(), "uefi_vars.fd").delete();
            setStatus(getString(R.string.vars_reset));
        });
        vmRamButton.setOnClickListener(v -> {
            long cur = prefs.getLong(VmActivity.PREF_RAM, 2048), next = VM_RAM_CHOICES[0];
            for (int i = 0; i < VM_RAM_CHOICES.length; i++)
                if (VM_RAM_CHOICES[i] == cur) next = VM_RAM_CHOICES[(i + 1) % VM_RAM_CHOICES.length];
            prefs.edit().putLong(VmActivity.PREF_RAM, next).apply();
            updateVmLabels();
        });
        updateVmLabels();
        input.setOnEditorActionListener((v, actionId, ev) -> {
            if (actionId == EditorInfo.IME_ACTION_SEND
                    || (ev != null && ev.getKeyCode() == KeyEvent.KEYCODE_ENTER && ev.getAction() == KeyEvent.ACTION_DOWN)) {
                sendLine();
                return true;
            }
            return false;
        });

        LinearLayout quick = findViewById(R.id.quick);
        for (String cmd : new String[] {"help", "info", "selftest", "bench", "mandel", "primes 200", "uptime"}) {
            Button b = new Button(this);
            b.setText(cmd);
            b.setAllCaps(false);
            b.setOnClickListener(v -> sendText(cmd + "\r"));
            quick.addView(b);
        }

        updateRamButton();
        setStatus(getString(R.string.status_idle, Engine.version()));
        appendText(getString(R.string.welcome));

        // Automation hook used by the CI smoke test:
        //   am start -n com.bxiidn.winddroid/.MainActivity --ez autorun true --es input 'selftest;poweroff'
        Intent it = getIntent();
        if (it != null && it.getBooleanExtra("autorun", false)) {
            logOutput = true;
            startDemo();
            String script = it.getStringExtra("input");
            if (script != null) {
                for (String cmd : script.split(";")) sendText(cmd.trim() + "\r");
            }
        }
    }

    @Override
    protected void onDestroy() {
        engine.stop();
        super.onDestroy();
    }

    private long ramMb() {
        return prefs.getLong("ram_mb", 256);
    }

    private void cycleRam() {
        long cur = ramMb();
        long next = RAM_CHOICES[0];
        for (int i = 0; i < RAM_CHOICES.length; i++) {
            if (RAM_CHOICES[i] == cur) {
                next = RAM_CHOICES[(i + 1) % RAM_CHOICES.length];
                break;
            }
        }
        prefs.edit().putLong("ram_mb", next).apply();
        updateRamButton();
    }

    private void updateRamButton() {
        ramButton.setText(getString(R.string.ram_label, ramMb()));
    }

    private void startDemo() {
        try (InputStream in = getAssets().open("monitor.elf")) {
            start(readAll(in), "WindDroid Monitor");
        } catch (IOException e) {
            showError(e.getMessage());
        }
    }

    private void pickImage() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, REQ_IMAGE);
    }

    // ------------------------------------------------------------ UEFI virtual machine

    private void updateVmLabels() {
        String disk = prefs.getString(VmActivity.PREF_DISK_NAME, null);
        String iso = prefs.getString(VmActivity.PREF_ISO_NAME, null);
        if (prefs.getString(VmActivity.PREF_DISK, null) == null) disk = null;
        if (prefs.getString(VmActivity.PREF_ISO, null) == null) iso = null;
        vmDisks.setText(getString(R.string.vm_disk_label, disk == null ? getString(R.string.none) : disk) + "\n"
                + getString(R.string.vm_iso_label, iso == null ? getString(R.string.none) : iso));
        vmRamButton.setText(getString(R.string.btn_vm_ram, prefs.getLong(VmActivity.PREF_RAM, 2048)));
        vmCpuButton.setText(getString(R.string.btn_vm_cpu, prefs.getInt(VmActivity.PREF_CPUS, 2)));
        vmNetButton.setText(prefs.getBoolean(VmActivity.PREF_NET, true) ? R.string.btn_vm_net_on : R.string.btn_vm_net_off);
        vmJitButton.setText(prefs.getBoolean(VmActivity.PREF_JIT, true) ? R.string.btn_vm_jit_on : R.string.btn_vm_jit_off);
    }

    private void startVm() {
        engine.stop();
        startActivity(new Intent(this, VmActivity.class));
    }

    private void pickDocument(int req) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
                | (req == REQ_DISK ? Intent.FLAG_GRANT_WRITE_URI_PERMISSION : 0));
        startActivityForResult(i, req);
    }

    private void forget(String key, String nameKey) {
        prefs.edit().remove(key).remove(nameKey).apply();
        updateVmLabels();
    }

    private void newDisk() {
        String[] labels = new String[NEWDISK_GB.length];
        for (int i = 0; i < labels.length; i++) labels[i] = NEWDISK_GB[i] + " GB";
        new AlertDialog.Builder(this)
                .setTitle(R.string.newdisk_title)
                .setItems(labels, (d, which) -> {
                    pendingNewDiskGb = NEWDISK_GB[which];
                    Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                    i.addCategory(Intent.CATEGORY_OPENABLE);
                    i.setType("application/octet-stream");
                    i.putExtra(Intent.EXTRA_TITLE, "windows-arm64.vhdx");
                    startActivityForResult(i, REQ_NEWDISK);
                })
                .show();
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (RuntimeException ignored) {
        }
        String s = uri.getLastPathSegment();
        return s == null ? "image" : s;
    }

    private static final String[] FORMATS = {"RAW", "ISO", "VHD", "VHDX"};

    private void onDocument(int req, Uri uri, int flags) {
        try {
            int take = Intent.FLAG_GRANT_READ_URI_PERMISSION | (req == REQ_ISO ? 0 : Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            getContentResolver().takePersistableUriPermission(uri, take & flags | Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException e) {
            Log.w(TAG, "no persistable permission", e);
        }
        String name = displayName(uri);
        if (req == REQ_NEWDISK) {
            try {
                ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "rw");
                if (pfd == null || !Engine.createVhdx(pfd.detachFd(), pendingNewDiskGb << 30)) throw new IOException();
                prefs.edit().putString(VmActivity.PREF_DISK, uri.toString()).putString(VmActivity.PREF_DISK_NAME, name).apply();
                setStatus(getString(R.string.newdisk_ok, name, pendingNewDiskGb));
            } catch (IOException | RuntimeException e) {
                showError(getString(R.string.newdisk_fail));
            }
            updateVmLabels();
            return;
        }
        String fmt = "?";
        try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r")) {
            if (pfd != null) fmt = FORMATS[Math.max(0, Math.min(3, Engine.imageFormat(pfd.getFd())))];
        } catch (IOException | RuntimeException e) {
            showError(String.valueOf(e.getMessage()));
            return;
        }
        String label = getString(R.string.disk_format, name, fmt);
        if (req == REQ_DISK) prefs.edit().putString(VmActivity.PREF_DISK, uri.toString()).putString(VmActivity.PREF_DISK_NAME, label).apply();
        else prefs.edit().putString(VmActivity.PREF_ISO, uri.toString()).putString(VmActivity.PREF_ISO_NAME, label).apply();
        updateVmLabels();
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res == RESULT_OK && data != null && data.getData() != null && req != REQ_IMAGE) {
            onDocument(req, data.getData(), data.getFlags());
            return;
        }
        if (req != REQ_IMAGE || res != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            if (in == null) throw new IOException("tidak bisa membuka file");
            String name = uri.getLastPathSegment();
            start(readAll(in), name == null ? "image" : name);
        } catch (IOException | OutOfMemoryError e) {
            showError(String.valueOf(e.getMessage()));
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[1 << 16];
        long total = 0;
        int n;
        while ((n = in.read(buf)) > 0) {
            total += n;
            if (total > MAX_IMAGE) throw new IOException("image lebih dari 512 MB");
            out.write(buf, 0, n);
        }
        return out.toByteArray();
    }

    private void start(byte[] image, String name) {
        try {
            imageName = name;
            parser.reset();
            appendText("\n--- " + getString(R.string.booting, name, ramMb()) + " ---\n");
            engine.start(image, ramMb(), this);
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            setStatus(getString(R.string.status_running, name));
        } catch (RuntimeException e) {
            showError(e.getMessage());
        }
    }

    private void sendLine() {
        String s = input.getText().toString();
        input.setText("");
        sendText(s + "\r");
    }

    private void sendText(String s) {
        if (!engine.isRunning()) {
            setStatus(getString(R.string.not_running));
            return;
        }
        engine.send(s.getBytes(StandardCharsets.UTF_8));
    }

    private void showError(String msg) {
        new AlertDialog.Builder(this)
                .setTitle(R.string.error_title)
                .setMessage(msg)
                .setPositiveButton(android.R.string.ok, null)
                .show();
    }

    private void setStatus(String s) {
        status.setText(s);
    }

    // ------------------------------------------------------------ terminal

    private void appendText(String s) {
        synchronized (term) {
            parser.feed(s, term);
            if (term.length() > MAX_TERMINAL_CHARS) {
                term.delete(0, term.length() - MAX_TERMINAL_CHARS);
                parser.resync(term);
            }
        }
        scheduleRender();
    }

    private void scheduleRender() {
        synchronized (term) {
            if (renderPosted) return;
            renderPosted = true;
        }
        ui.postDelayed(this::render, 30);
    }

    private void render() {
        String text;
        synchronized (term) {
            renderPosted = false;
            text = term.toString();
        }
        terminal.setText(text);
        scroll.post(() -> scroll.fullScroll(View.FOCUS_DOWN));
    }

    // ------------------------------------------------------------ Engine.Listener (VM thread)

    @Override
    public void onOutput(byte[] data) {
        String s = new String(data, StandardCharsets.ISO_8859_1);
        if (logOutput) {
            for (int i = 0; i < s.length(); i++) {
                char c = s.charAt(i);
                if (c == '\n') {
                    Log.i(TAG, logLine.toString());
                    logLine.setLength(0);
                } else if (c != '\r') {
                    logLine.append(c);
                }
            }
        }
        appendText(s);
    }

    @Override
    public void onStats(long instructions, double mips) {
        ui.post(() -> {
            if (engine.isRunning())
                setStatus(String.format(Locale.US, "%s  |  %,d instruksi  |  %.1f MIPS", imageName, instructions, mips));
        });
    }

    @Override
    public void onStopped(int st, int exitCode, String detail) {
        ui.post(() -> {
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            String msg;
            switch (st) {
                case Engine.RUN_SHUTDOWN: msg = getString(R.string.stopped_shutdown, exitCode); break;
                case Engine.RUN_RESET: msg = getString(R.string.stopped_reset); break;
                case Engine.RUN_ERROR: msg = getString(R.string.stopped_error); break;
                default: msg = getString(R.string.stopped_user); break;
            }
            appendText("\n--- " + msg + " ---\n");
            if (!detail.isEmpty()) appendText(detail + "\n");
            if (logOutput) Log.i(TAG, "STOPPED status=" + st + " exit=" + exitCode + " " + msg + " " + detail);
            setStatus(msg);
        });
    }

    /** Minimal VT100 handling: CR/LF, backspace, clear screen; other escapes are dropped. */
    static final class TerminalParser {
        private int esc; // 0 none, 1 after ESC, 2 inside CSI
        private final StringBuilder csi = new StringBuilder();
        private int lineStart = 0;
        private boolean pendingCr;

        void reset() {
            esc = 0;
            csi.setLength(0);
            lineStart = 0;
            pendingCr = false;
        }

        void feed(String s, StringBuilder out) {
            if (lineStart > out.length()) lineStart = lastLineStart(out);
            for (int i = 0; i < s.length(); i++) {
                char c = s.charAt(i);
                if (esc == 1) {
                    esc = c == '[' ? 2 : 0;
                    csi.setLength(0);
                    continue;
                }
                if (esc == 2) {
                    if (c >= 0x40 && c <= 0x7E) {
                        if (c == 'J' && csi.toString().equals("2")) {
                            out.setLength(0);
                            lineStart = 0;
                        }
                        esc = 0;
                    } else {
                        csi.append(c);
                    }
                    continue;
                }
                switch (c) {
                    case 27: esc = 1; break;
                    case '\r': pendingCr = true; break;
                    case '\n':
                        out.append('\n');
                        lineStart = out.length();
                        pendingCr = false;
                        break;
                    case '\b':
                        if (out.length() > lineStart) out.setLength(out.length() - 1);
                        break;
                    default:
                        if (pendingCr) {
                            out.setLength(lineStart);
                            pendingCr = false;
                        }
                        if (c >= 32 || c == '\t') out.append(c);
                }
            }
        }

        void resync(StringBuilder out) {
            lineStart = lastLineStart(out);
        }

        private static int lastLineStart(StringBuilder out) {
            int i = out.lastIndexOf("\n");
            return i < 0 ? 0 : i + 1;
        }
    }
}
