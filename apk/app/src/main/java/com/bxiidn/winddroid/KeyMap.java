package com.bxiidn.winddroid;

import android.util.SparseIntArray;
import android.view.KeyEvent;

/** Android key codes to USB HID keyboard usages (HID Usage Tables, page 0x07). */
final class KeyMap {
    private static final SparseIntArray MAP = new SparseIntArray();

    static {
        for (int i = 0; i < 26; i++) MAP.put(KeyEvent.KEYCODE_A + i, 0x04 + i);
        for (int i = 1; i <= 9; i++) MAP.put(KeyEvent.KEYCODE_1 + i - 1, 0x1E + i - 1);
        MAP.put(KeyEvent.KEYCODE_0, 0x27);
        MAP.put(KeyEvent.KEYCODE_ENTER, 0x28);
        MAP.put(KeyEvent.KEYCODE_ESCAPE, 0x29);
        MAP.put(KeyEvent.KEYCODE_DEL, 0x2A);
        MAP.put(KeyEvent.KEYCODE_TAB, 0x2B);
        MAP.put(KeyEvent.KEYCODE_SPACE, 0x2C);
        MAP.put(KeyEvent.KEYCODE_MINUS, 0x2D);
        MAP.put(KeyEvent.KEYCODE_EQUALS, 0x2E);
        MAP.put(KeyEvent.KEYCODE_LEFT_BRACKET, 0x2F);
        MAP.put(KeyEvent.KEYCODE_RIGHT_BRACKET, 0x30);
        MAP.put(KeyEvent.KEYCODE_BACKSLASH, 0x31);
        MAP.put(KeyEvent.KEYCODE_SEMICOLON, 0x33);
        MAP.put(KeyEvent.KEYCODE_APOSTROPHE, 0x34);
        MAP.put(KeyEvent.KEYCODE_GRAVE, 0x35);
        MAP.put(KeyEvent.KEYCODE_COMMA, 0x36);
        MAP.put(KeyEvent.KEYCODE_PERIOD, 0x37);
        MAP.put(KeyEvent.KEYCODE_SLASH, 0x38);
        MAP.put(KeyEvent.KEYCODE_CAPS_LOCK, 0x39);
        for (int i = 0; i < 12; i++) MAP.put(KeyEvent.KEYCODE_F1 + i, 0x3A + i);
        MAP.put(KeyEvent.KEYCODE_SYSRQ, 0x46);
        MAP.put(KeyEvent.KEYCODE_SCROLL_LOCK, 0x47);
        MAP.put(KeyEvent.KEYCODE_BREAK, 0x48);
        MAP.put(KeyEvent.KEYCODE_INSERT, 0x49);
        MAP.put(KeyEvent.KEYCODE_MOVE_HOME, 0x4A);
        MAP.put(KeyEvent.KEYCODE_PAGE_UP, 0x4B);
        MAP.put(KeyEvent.KEYCODE_FORWARD_DEL, 0x4C);
        MAP.put(KeyEvent.KEYCODE_MOVE_END, 0x4D);
        MAP.put(KeyEvent.KEYCODE_PAGE_DOWN, 0x4E);
        MAP.put(KeyEvent.KEYCODE_DPAD_RIGHT, 0x4F);
        MAP.put(KeyEvent.KEYCODE_DPAD_LEFT, 0x50);
        MAP.put(KeyEvent.KEYCODE_DPAD_DOWN, 0x51);
        MAP.put(KeyEvent.KEYCODE_DPAD_UP, 0x52);
        MAP.put(KeyEvent.KEYCODE_NUM_LOCK, 0x53);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_DIVIDE, 0x54);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_MULTIPLY, 0x55);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_SUBTRACT, 0x56);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_ADD, 0x57);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_ENTER, 0x58);
        for (int i = 1; i <= 9; i++) MAP.put(KeyEvent.KEYCODE_NUMPAD_1 + i - 1, 0x59 + i - 1);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_0, 0x62);
        MAP.put(KeyEvent.KEYCODE_NUMPAD_DOT, 0x63);
        MAP.put(KeyEvent.KEYCODE_MENU, 0x65);
        MAP.put(KeyEvent.KEYCODE_CTRL_LEFT, 0xE0);
        MAP.put(KeyEvent.KEYCODE_SHIFT_LEFT, 0xE1);
        MAP.put(KeyEvent.KEYCODE_ALT_LEFT, 0xE2);
        MAP.put(KeyEvent.KEYCODE_META_LEFT, 0xE3);
        MAP.put(KeyEvent.KEYCODE_CTRL_RIGHT, 0xE4);
        MAP.put(KeyEvent.KEYCODE_SHIFT_RIGHT, 0xE5);
        MAP.put(KeyEvent.KEYCODE_ALT_RIGHT, 0xE6);
        MAP.put(KeyEvent.KEYCODE_META_RIGHT, 0xE7);
    }

    static final int CTRL = 0xE0, SHIFT = 0xE1, ALT = 0xE2, GUI = 0xE3;
    static final int ESC = 0x29, TAB = 0x2B, DELETE = 0x4C, F2 = 0x3B, F12 = 0x45;
    static final int LEFT = 0x50, RIGHT = 0x4F, UP = 0x52, DOWN = 0x51, ENTER = 0x28;

    static int usage(int keyCode) {
        return MAP.get(keyCode, 0);
    }

    private KeyMap() {}
}
