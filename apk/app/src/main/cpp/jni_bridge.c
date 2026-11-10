/*
 * JNI bridge between com.bxiidn.winddroid.Engine and the WindDroid engine.
 * All calls for one machine must come from the same thread (the VM thread).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <jni.h>
#include <stdlib.h>
#include <string.h>

#include "winddroid.h"

typedef struct {
    wd_machine *m;
    uint8_t *out;
    size_t out_len, out_cap;
} handle;

static void on_tx(void *opaque, uint8_t ch)
{
    handle *h = opaque;
    if (h->out_len == h->out_cap) {
        size_t cap = h->out_cap ? h->out_cap * 2 : 4096;
        if (cap > (1u << 24)) return;                  /* drop output beyond 16 MiB backlog */
        uint8_t *n = realloc(h->out, cap);
        if (!n) return;
        h->out = n;
        h->out_cap = cap;
    }
    h->out[h->out_len++] = ch;
}

static handle *H(jlong p) { return (handle *)(intptr_t)p; }

JNIEXPORT jlong JNICALL Java_com_bxiidn_winddroid_Engine_nativeCreate(JNIEnv *env, jclass cls, jlong ram_mb,
                                                                       jint cpus, jboolean realtime)
{
    handle *h = calloc(1, sizeof *h);
    if (!h) return 0;
    wd_config cfg = { .ram_size = (uint64_t)ram_mb << 20, .realtime = realtime ? 1 : 0, .ncpu = cpus };
    h->m = wd_create(&cfg);
    if (!h->m) { free(h); return 0; }
    wd_set_uart_tx(h->m, on_tx, h);
    return (jlong)(intptr_t)h;
}

JNIEXPORT void JNICALL Java_com_bxiidn_winddroid_Engine_nativeDestroy(JNIEnv *env, jclass cls, jlong p)
{
    handle *h = H(p);
    if (!h) return;
    wd_destroy(h->m);
    free(h->out);
    free(h);
}

JNIEXPORT jint JNICALL Java_com_bxiidn_winddroid_Engine_nativeLoad(JNIEnv *env, jclass cls, jlong p, jbyteArray img)
{
    handle *h = H(p);
    jsize n = (*env)->GetArrayLength(env, img);
    jbyte *b = (*env)->GetByteArrayElements(env, img, NULL);
    if (!b) return -1;
    int r = wd_load_image(h->m, (const uint8_t *)b, (size_t)n);
    (*env)->ReleaseByteArrayElements(env, img, b, JNI_ABORT);
    return r;
}

JNIEXPORT jint JNICALL Java_com_bxiidn_winddroid_Engine_nativeRun(JNIEnv *env, jclass cls, jlong p, jlong max)
{
    return (jint)wd_run(H(p)->m, (uint64_t)max);
}

JNIEXPORT jlong JNICALL Java_com_bxiidn_winddroid_Engine_nativeIdleNs(JNIEnv *env, jclass cls, jlong p)
{
    uint64_t ns = wd_idle_ns(H(p)->m);
    return ns > (uint64_t)INT64_MAX ? -1 : (jlong)ns;
}

JNIEXPORT void JNICALL Java_com_bxiidn_winddroid_Engine_nativeInput(JNIEnv *env, jclass cls, jlong p, jbyteArray data)
{
    jsize n = (*env)->GetArrayLength(env, data);
    jbyte *b = (*env)->GetByteArrayElements(env, data, NULL);
    if (!b) return;
    wd_uart_input(H(p)->m, (const uint8_t *)b, (size_t)n);
    (*env)->ReleaseByteArrayElements(env, data, b, JNI_ABORT);
}

JNIEXPORT jbyteArray JNICALL Java_com_bxiidn_winddroid_Engine_nativeTakeOutput(JNIEnv *env, jclass cls, jlong p)
{
    handle *h = H(p);
    if (!h->out_len) return NULL;
    jbyteArray a = (*env)->NewByteArray(env, (jsize)h->out_len);
    if (a) (*env)->SetByteArrayRegion(env, a, 0, (jsize)h->out_len, (const jbyte *)h->out);
    h->out_len = 0;
    return a;
}

JNIEXPORT jlong JNICALL Java_com_bxiidn_winddroid_Engine_nativeIcount(JNIEnv *env, jclass cls, jlong p)
{
    return (jlong)wd_icount(H(p)->m);
}

JNIEXPORT jint JNICALL Java_com_bxiidn_winddroid_Engine_nativeExitCode(JNIEnv *env, jclass cls, jlong p)
{
    return wd_exit_code(H(p)->m);
}

JNIEXPORT jstring JNICALL Java_com_bxiidn_winddroid_Engine_nativeError(JNIEnv *env, jclass cls, jlong p)
{
    return (*env)->NewStringUTF(env, wd_error(H(p)->m));
}

JNIEXPORT jstring JNICALL Java_com_bxiidn_winddroid_Engine_nativeState(JNIEnv *env, jclass cls, jlong p)
{
    char buf[2048];
    wd_dump_state(H(p)->m, buf, sizeof buf);
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jstring JNICALL Java_com_bxiidn_winddroid_Engine_nativeDiag(JNIEnv *env, jclass cls, jlong p)
{
    char *buf = malloc(32768);
    if (!buf) return NULL;
    wd_diag_report(H(p)->m, buf, 32768);
    for (char *q = buf; *q; q++) if ((unsigned char)*q >= 0x80) *q = '?';   /* NewStringUTF wants valid UTF-8 */
    jstring r = (*env)->NewStringUTF(env, buf);
    free(buf);
    return r;
}

JNIEXPORT jstring JNICALL Java_com_bxiidn_winddroid_Engine_nativeVersion(JNIEnv *env, jclass cls)
{
    return (*env)->NewStringUTF(env, WD_VERSION);
}

/* ---------------------------------------------------------------- UEFI machine */

#include <android/bitmap.h>
#include <unistd.h>

JNIEXPORT jstring JNICALL Java_com_bxiidn_winddroid_Engine_nativeLoadFirmware(JNIEnv *env, jclass cls, jlong p,
                                                                              jbyteArray code, jbyteArray vars)
{
    handle *h = H(p);
    jsize cn = (*env)->GetArrayLength(env, code);
    jbyte *cb = (*env)->GetByteArrayElements(env, code, NULL);
    jsize vn = vars ? (*env)->GetArrayLength(env, vars) : 0;
    jbyte *vb = vars ? (*env)->GetByteArrayElements(env, vars, NULL) : NULL;
    int r = cb ? wd_load_firmware(h->m, (const uint8_t *)cb, (size_t)cn, (const uint8_t *)vb, (size_t)vn) : -1;
    if (cb) (*env)->ReleaseByteArrayElements(env, code, cb, JNI_ABORT);
    if (vb) (*env)->ReleaseByteArrayElements(env, vars, vb, JNI_ABORT);
    return r ? (*env)->NewStringUTF(env, wd_error(h->m)) : NULL;
}

/* kind: 0 NVMe SSD, 1 USB disk, 2 USB DVD drive. The engine owns fd afterwards (also on failure). */
JNIEXPORT jstring JNICALL Java_com_bxiidn_winddroid_Engine_nativeAttachDisk(JNIEnv *env, jclass cls, jlong p, jint fd,
                                                                            jint kind, jboolean readonly)
{
    handle *h = H(p);
    wd_disk d;
    char err[256];
    if (wd_disk_open_image(fd, readonly || kind == 2, 1, &d, err, sizeof err)) {
        close(fd);
        return (*env)->NewStringUTF(env, err);
    }
    int r;
    if (kind == 0) r = wd_attach_nvme(h->m, &d);
    else r = wd_attach_usb_storage(h->m, &d, kind == 2);
    if (r) {
        d.close(d.opaque);
        return (*env)->NewStringUTF(env, wd_error(h->m));
    }
    return NULL;
}

JNIEXPORT jint JNICALL Java_com_bxiidn_winddroid_Engine_nativeImageFormat(JNIEnv *env, jclass cls, jint fd)
{
    return wd_image_probe(fd);
}

JNIEXPORT jint JNICALL Java_com_bxiidn_winddroid_Engine_nativeCreateVhdx(JNIEnv *env, jclass cls, jint fd, jlong size)
{
    int r = wd_vhdx_create(fd, (uint64_t)size, 0, 0);
    close(fd);
    return r;
}

JNIEXPORT jint JNICALL Java_com_bxiidn_winddroid_Engine_nativeAttachUsbInput(JNIEnv *env, jclass cls, jlong p)
{
    handle *h = H(p);
    return wd_attach_usb_keyboard(h->m) || wd_attach_usb_tablet(h->m) ? -1 : 0;
}

JNIEXPORT jint JNICALL Java_com_bxiidn_winddroid_Engine_nativeAttachUsbNet(JNIEnv *env, jclass cls, jlong p)
{
    return wd_attach_usb_net(H(p)->m, NULL);
}

JNIEXPORT void JNICALL Java_com_bxiidn_winddroid_Engine_nativeKey(JNIEnv *env, jclass cls, jlong p, jint usage, jboolean down)
{
    wd_key_event(H(p)->m, usage, down ? 1 : 0);
}

JNIEXPORT void JNICALL Java_com_bxiidn_winddroid_Engine_nativePointer(JNIEnv *env, jclass cls, jlong p, jint x, jint y,
                                                                      jint buttons, jint wheel)
{
    wd_pointer_event(H(p)->m, x, y, buttons, wheel);
}

JNIEXPORT jint JNICALL Java_com_bxiidn_winddroid_Engine_nativeAsciiToHid(JNIEnv *env, jclass cls, jint ch)
{
    int shift, u = wd_ascii_to_hid(ch, &shift);
    return u | (shift ? 0x100 : 0);
}

/* out = { width, height, generation }; returns 0 while no mode is set */
JNIEXPORT jint JNICALL Java_com_bxiidn_winddroid_Engine_nativeFbInfo(JNIEnv *env, jclass cls, jlong p, jintArray out)
{
    wd_fb_info fb;
    if (!wd_framebuffer(H(p)->m, &fb)) return 0;
    jint v[3] = { (jint)fb.width, (jint)fb.height, (jint)fb.generation };
    (*env)->SetIntArrayRegion(env, out, 0, 3, v);
    return 1;
}

/* Copy the guest framebuffer (XRGB8888, B,G,R,X in memory) into an ARGB_8888 bitmap (R,G,B,A). */
JNIEXPORT jint JNICALL Java_com_bxiidn_winddroid_Engine_nativeBlit(JNIEnv *env, jclass cls, jlong p, jobject bitmap)
{
    wd_fb_info fb;
    if (!wd_framebuffer(H(p)->m, &fb)) return 0;
    AndroidBitmapInfo bi;
    void *px;
    if (AndroidBitmap_getInfo(env, bitmap, &bi) != ANDROID_BITMAP_RESULT_SUCCESS ||
        bi.format != ANDROID_BITMAP_FORMAT_RGBA_8888 || bi.width != fb.width || bi.height != fb.height)
        return 0;
    if (AndroidBitmap_lockPixels(env, bitmap, &px) != ANDROID_BITMAP_RESULT_SUCCESS) return 0;
    for (uint32_t y = 0; y < fb.height; y++) {
        const uint32_t *src = (const uint32_t *)(fb.pixels + (size_t)y * fb.stride);
        uint32_t *dst = (uint32_t *)((uint8_t *)px + (size_t)y * bi.stride);
        for (uint32_t x = 0; x < fb.width; x++) {
            uint32_t v = src[x];                                   /* 0xXXRRGGBB */
            dst[x] = 0xFF000000u | (v & 0x0000FF00u) | (v >> 16 & 0xFFu) | (v & 0xFFu) << 16;   /* 0xAABBGGRR */
        }
    }
    AndroidBitmap_unlockPixels(env, bitmap);
    return 1;
}

/* The variable store when it changed since the last call, else null. */
JNIEXPORT jbyteArray JNICALL Java_com_bxiidn_winddroid_Engine_nativeTakeVars(JNIEnv *env, jclass cls, jlong p, jboolean force)
{
    handle *h = H(p);
    size_t len;
    int dirty;
    const uint8_t *v = wd_firmware_vars(h->m, &len, &dirty);
    if (!v || (!dirty && !force)) return NULL;
    jbyteArray a = (*env)->NewByteArray(env, (jsize)len);
    if (a) {
        (*env)->SetByteArrayRegion(env, a, 0, (jsize)len, (const jbyte *)v);
        wd_firmware_vars_saved(h->m);
    }
    return a;
}

JNIEXPORT jboolean JNICALL Java_com_bxiidn_winddroid_Engine_nativeJitActive(JNIEnv *env, jclass cls, jlong p)
{
    return wd_jit_active(H(p)->m) ? JNI_TRUE : JNI_FALSE;
}
