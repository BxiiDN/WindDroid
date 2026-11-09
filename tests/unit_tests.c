/*
 * Host-side unit tests for engine pieces that are easiest to check directly:
 * the logical-immediate decoder (cross-checked against the LLVM assembler's
 * encodings in tests/bitmask_vectors.h) and the ELF loader's error handling.
 */
#include <stdio.h>
#include <stdlib.h>

#include "internal.h"

int wd_decode_bitmask(int sf, int N, int imms, int immr, uint64_t *out);

#include "bitmask_vectors.h"

static int failures;
#define T(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main(void)
{
    /* every encodable 64-bit and 32-bit logical immediate */
    unsigned n64 = 0, n32 = 0;
    for (unsigned i = 0; i < sizeof bitmask_vectors / sizeof bitmask_vectors[0]; i++) {
        const struct bm *v = &bitmask_vectors[i];
        uint64_t got = 0;
        int ok = wd_decode_bitmask(v->sf, v->n, v->imms, v->immr, &got);
        if (!ok || got != v->value) {
            failures++;
            if (failures < 10)
                printf("FAIL bitmask sf=%d N=%d immr=%d imms=%d: got %llx want %llx\n", v->sf, v->n, v->immr,
                       v->imms, (unsigned long long)got, (unsigned long long)v->value);
        }
        if (v->sf) n64++; else n32++;
    }
    T(n64 == 7680);
    T(n32 == 992);
    /* and nothing else is accepted */
    unsigned accepted = 0;
    for (int sf = 0; sf < 2; sf++)
        for (int N = 0; N < 2; N++)
            for (int immr = 0; immr < 64; immr++)
                for (int imms = 0; imms < 64; imms++) {
                    if (!sf && (immr > 31 || imms > 31)) continue;
                    uint64_t v;
                    accepted += (unsigned)wd_decode_bitmask(sf, N, imms, immr, &v);
                }
    T(accepted == n64 + n32);
    /* reserved encodings */
    uint64_t dummy;
    T(!wd_decode_bitmask(1, 0, 0x3F, 0, &dummy));     /* all ones element */
    T(!wd_decode_bitmask(0, 1, 0, 0, &dummy));        /* N=1 for 32-bit */
    T(!wd_decode_bitmask(1, 1, 0x3F, 0, &dummy));

    /* loader rejects bad input */
    wd_config cfg = { .ram_size = 16 << 20 };
    wd_machine *m = wd_create(&cfg);
    T(m != NULL);
    uint8_t junk[100] = { 0x7f, 'E', 'L', 'F', 2, 1 };
    T(wd_load_elf(m, junk, sizeof junk) == -1);
    T(wd_load_raw(m, junk, sizeof junk, 0x1000) == -1);
    T(wd_load_raw(m, junk, sizeof junk, WD_RAM_BASE + (16 << 20) - 50) == -1);
    T(wd_load_raw(m, junk, sizeof junk, WD_RAM_BASE) == 0);

    /* a tiny raw program: mov x0,#7 ; str w0,[ctl] -> exit(7) */
    uint32_t prog[] = {
        0xD28000E0,             /* mov x0, #7 */
        0xD2A12201,             /* mov x1, #0x09100000 */
        0xB9000020,             /* str w0, [x1] */
        0x14000000,             /* b . */
    };
    T(wd_load_image(m, (const uint8_t *)prog, sizeof prog) == 0);
    T(wd_run(m, 100) == WD_RUN_SHUTDOWN);
    T(wd_exit_code(m) == 7);
    T(wd_icount(m) == 3);
    wd_destroy(m);

    printf("unit tests: %d failures (%u + %u bitmask vectors)\n", failures, n64, n32);
    return failures ? 1 : 0;
}
