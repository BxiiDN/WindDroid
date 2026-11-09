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
                                                                       jboolean realtime)
{
    handle *h = calloc(1, sizeof *h);
    if (!h) return 0;
    wd_config cfg = { .ram_size = (uint64_t)ram_mb << 20, .realtime = realtime ? 1 : 0 };
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

JNIEXPORT jstring JNICALL Java_com_bxiidn_winddroid_Engine_nativeVersion(JNIEnv *env, jclass cls)
{
    return (*env)->NewStringUTF(env, WD_VERSION);
}
