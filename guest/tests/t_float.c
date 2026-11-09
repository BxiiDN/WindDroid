/* Compiler-generated floating point (built with -O3 and -ffp-contract=off so the
 * operation order is fixed). tests/float_ref.py computes the same values in Python. */
#include "rt.h"

#define N 24
static float fa[N][N], fb[N][N], fc[N][N];
static double da[N][N], db[N][N], dc[N][N];

static uint64_t h64(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001B3ULL; }
static uint64_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

int main(void)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            fa[i][j] = (float)(i * 3 + j) / 7.0f - 1.5f;
            fb[i][j] = (float)(j * 5 - i) * 0.125f + 0.3f;
            da[i][j] = (double)(i * 3 + j) / 7.0 - 1.5;
            db[i][j] = (double)(j * 5 - i) * 0.125 + 0.3;
        }
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            float s = 0.0f;
            double t = 0.0;
            for (int k = 0; k < N; k++) {
                s = s + fa[i][k] * fb[k][j];
                t = t + da[i][k] * db[k][j];
            }
            fc[i][j] = s;
            dc[i][j] = t;
        }
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) { h = h64(h, fbits(fc[i][j])); h = h64(h, dbits(dc[i][j])); }
    printf_("matmul %016llx\n", (unsigned long long)h);

    /* element-wise vectorisable loops */
    static float v[256], w[256];
    for (int i = 0; i < 256; i++) { v[i] = (float)i * 0.37f - 40.0f; w[i] = 1.0f / ((float)i + 0.5f); }
    for (int i = 0; i < 256; i++) v[i] = v[i] * w[i] + (v[i] > 0.0f ? v[i] : -v[i]);
    h = 0xCBF29CE484222325ULL;
    for (int i = 0; i < 256; i++) h = h64(h, fbits(v[i]));
    printf_("vec %016llx\n", (unsigned long long)h);

    /* Newton square roots, polynomial, conversions */
    h = 0xCBF29CE484222325ULL;
    for (int i = 1; i < 200; i++) {
        double x = (double)i * 1.75, r = x;
        for (int k = 0; k < 8; k++) r = 0.5 * (r + x / r);
        double p = ((0.25 * x - 1.5) * x + 3.0) * x - 7.0;
        long long ip = (long long)p;
        float sf = (float)r;
        h = h64(h, dbits(r));
        h = h64(h, dbits(p));
        h = h64(h, (uint64_t)ip);
        h = h64(h, fbits(sf));
        h = h64(h, (uint64_t)(int)(sf * 3.9f));
    }
    printf_("newton %016llx\n", (unsigned long long)h);
    return 0;
}
