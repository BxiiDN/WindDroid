package com.bxiidn.winddroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
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
    private static final int REQ_IMAGE = 1;
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

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
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
