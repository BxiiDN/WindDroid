/* SIMD stores and structure loads/stores, all addressing forms. */
#include "rt.h"

static uint8_t src[128] __attribute__((aligned(16)));
static uint8_t dst[160] __attribute__((aligned(16)));

static void fill(void) { for (int i = 0; i < 128; i++) src[i] = (uint8_t)(i * 7 + 3); memset(dst, 0xEE, sizeof dst); }

int main(void)
{
    uint8_t *p;

    /* ST1 / LD1 with one to four registers, post-index by immediate and register */
    fill();
    p = dst;
    __asm__ volatile("ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [%1]\n st1 {v0.16b, v1.16b, v2.16b, v3.16b}, [%0], #64"
                     : "+r"(p) : "r"(src) : "v0", "v1", "v2", "v3", "memory");
    CHECK_EQ(memcmp(dst, src, 64), 0);
    CHECK_EQ(p, dst + 64);
    CHECK_EQ(dst[64], 0xEE);
    fill();
    uint64_t step = 24;
    p = dst;
    __asm__ volatile("ld1 {v4.8b, v5.8b, v6.8b}, [%2]\n st1 {v4.8b, v5.8b, v6.8b}, [%0], %1"
                     : "+r"(p) : "r"(step), "r"(src) : "v4", "v5", "v6", "memory");
    CHECK_EQ(memcmp(dst, src, 24), 0);
    CHECK_EQ(p, dst + 24);
    CHECK_EQ(dst[24], 0xEE);

    /* ST2: interleave two halfword vectors; LD2 undoes it */
    fill();
    __asm__ volatile("ld1 {v0.8h}, [%1]\n ld1 {v1.8h}, [%2]\n st2 {v0.8h, v1.8h}, [%0]"
                     :: "r"(dst), "r"(src), "r"(src + 16) : "v0", "v1", "memory");
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        if (memcmp(dst + 4 * i, src + 2 * i, 2) || memcmp(dst + 4 * i + 2, src + 16 + 2 * i, 2)) ok = 0;
    }
    CHECK(ok);
    uint8_t back[32];
    __asm__ volatile("ld2 {v2.8h, v3.8h}, [%0]\n st1 {v2.8h, v3.8h}, [%1]" :: "r"(dst), "r"(back) : "v2", "v3", "memory");
    CHECK_EQ(memcmp(back, src, 32), 0);

    /* ST3 / ST4 bytes */
    fill();
    __asm__ volatile("ld1 {v0.16b, v1.16b, v2.16b}, [%1]\n st3 {v0.16b, v1.16b, v2.16b}, [%0]"
                     :: "r"(dst), "r"(src) : "v0", "v1", "v2", "memory");
    ok = 1;
    for (int i = 0; i < 16; i++)
        for (int r = 0; r < 3; r++) if (dst[3 * i + r] != src[16 * r + i]) ok = 0;
    CHECK(ok);
    fill();
    __asm__ volatile("ld1 {v0.8b, v1.8b, v2.8b, v3.8b}, [%1]\n st4 {v0.8b, v1.8b, v2.8b, v3.8b}, [%0]"
                     :: "r"(dst), "r"(src) : "v0", "v1", "v2", "v3", "memory");
    ok = 1;
    for (int i = 0; i < 8; i++)
        for (int r = 0; r < 4; r++) if (dst[4 * i + r] != src[8 * r + i]) ok = 0;
    CHECK(ok);
    CHECK_EQ(dst[32], 0xEE);

    /* single lane stores and loads with post-index */
    fill();
    p = dst + 3;
    __asm__ volatile("ld1 {v7.4s}, [%1]\n st1 {v7.s}[2], [%0], #4" : "+r"(p) : "r"(src) : "v7", "memory");
    CHECK_EQ(memcmp(dst + 3, src + 8, 4), 0);
    CHECK_EQ(p, dst + 7);
    p = dst;
    __asm__ volatile("ld1 {v0.16b, v1.16b}, [%1]\n st2 {v0.h, v1.h}[5], [%0], #4" : "+r"(p) : "r"(src) : "v0", "v1", "memory");
    CHECK_EQ(memcmp(dst, src + 10, 2), 0);
    CHECK_EQ(memcmp(dst + 2, src + 26, 2), 0);
    CHECK_EQ(p, dst + 4);
    p = (uint8_t *)src;
    uint64_t lo;
    __asm__ volatile("movi v9.16b, #0\n ld1r {v9.8h}, [%0], #2\n umov %1, v9.d[1]" : "+r"(p), "=r"(lo) :: "v9", "memory");
    uint16_t h0;
    memcpy(&h0, src, 2);
    CHECK_EQ(lo, (uint64_t)h0 * 0x0001000100010001ULL);
    CHECK_EQ(p, src + 2);

    /* STR/LDR Q with register offset, pre/post index; STP/LDP Q/D/S */
    fill();
    uint64_t idx = 2;
    __asm__ volatile("ldr q10, [%1]\n str q10, [%0, %2, lsl #4]" :: "r"(dst), "r"(src), "r"(idx) : "v10", "memory");
    CHECK_EQ(memcmp(dst + 32, src, 16), 0);
    p = dst + 64;
    __asm__ volatile("ldp q11, q12, [%1]\n stp q11, q12, [%0, #-32]!" : "+r"(p) : "r"(src + 32) : "v11", "v12", "memory");
    CHECK_EQ(p, dst + 32);
    CHECK_EQ(memcmp(dst + 32, src + 32, 32), 0);
    p = dst;
    __asm__ volatile("ldp d13, d14, [%1]\n stp d13, d14, [%0], #16" : "+r"(p) : "r"(src + 5) : "v13", "v14", "memory");
    CHECK_EQ(memcmp(dst, src + 5, 16), 0);
    CHECK_EQ(p, dst + 16);
    __asm__ volatile("ldp s13, s14, [%1, #8]\n stp s13, s14, [%0, #4]" :: "r"(dst), "r"(src) : "v13", "v14", "memory");
    CHECK_EQ(memcmp(dst + 4, src + 8, 8), 0);
    p = dst + 100;
    __asm__ volatile("ldr h15, [%1]\n str h15, [%0], #-2\n ldr b16, [%1, #1]\n strb w0, [%0]\n str b16, [%0, #-1]!"
                     : "+r"(p) : "r"(src + 40) : "v15", "v16", "x0", "memory");
    CHECK_EQ(memcmp(dst + 100, src + 40, 2), 0);
    CHECK_EQ(dst[97], src[41]);
    CHECK_EQ(p, dst + 97);
    /* unscaled and literal */
    uint64_t lit;
    __asm__ volatile("ldr d17, 1f\n fmov %0, d17\n b 2f\n .balign 8\n1: .quad 0x1122334455667788\n2:" : "=r"(lit) :: "v17");
    CHECK_EQ(lit, 0x1122334455667788ULL);
    __asm__ volatile("ldur q18, [%1, #-3]\n stur q18, [%0, #-1]" :: "r"(dst + 120), "r"(src + 70) : "v18", "memory");
    CHECK_EQ(memcmp(dst + 119, src + 67, 16), 0);

    /* a store crossing a page boundary */
    static uint8_t pg[8192] __attribute__((aligned(4096)));
    __asm__ volatile("ldr q19, [%1]\n str q19, [%0]" :: "r"(pg + 4096 - 7), "r"(src) : "v19", "memory");
    CHECK_EQ(memcmp(pg + 4096 - 7, src, 16), 0);

    printf_("t_simdmem: %d failures\n", test_failures);
    return test_failures ? 1 : 0;
}
