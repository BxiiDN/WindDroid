/* Exceptions, system registers, PSCI, exclusives and LSE atomics at EL1. */
#include "rt.h"

static volatile int last_kind = -1;
static volatile uint64_t last_esr, last_far, last_elr;
static volatile int skip;            /* advance ELR by 4 after handling */

static int hook(struct frame *f, int kind)
{
    last_kind = kind;
    last_esr = READ_SYS(esr_el1);
    last_far = READ_SYS(far_el1);
    last_elr = f->elr;
    if (skip) f->elr += 4;
    return 1;
}

static uint64_t hvc(uint64_t fn, uint64_t a1)
{
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a1;
    __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1) : "x2", "x3", "memory");
    return x0;
}

static void test_exceptions(void)
{
    uint64_t pc;
    exc_hook = hook;

    /* SVC: preferred return is the next instruction, ISS = imm16 */
    skip = 0;
    __asm__ volatile("adr %0, 1f\n svc #0x123\n 1:" : "=r"(pc) :: "memory");
    CHECK_EQ(last_kind, 4);
    CHECK_EQ(last_esr >> 26, 0x15);
    CHECK_EQ(last_esr & 0xFFFF, 0x123);
    CHECK_EQ((last_esr >> 25) & 1, 1);
    CHECK_EQ(last_elr, pc);

    /* BRK: ELR points at the BRK itself */
    skip = 1;
    __asm__ volatile("adr %0, 1f\n 1: brk #7" : "=r"(pc) :: "memory");
    CHECK_EQ(last_esr >> 26, 0x3C);
    CHECK_EQ(last_esr & 0xFFFF, 7);
    CHECK_EQ(last_elr, pc);

    /* permanently undefined and FP/SIMD (not implemented) */
    __asm__ volatile("adr %0, 1f\n 1: udf #0" : "=r"(pc) :: "memory");
    CHECK_EQ(last_esr >> 26, 0);
    CHECK_EQ(last_elr, pc);
    /* FP/SIMD access traps with EC 0x07 while CPACR_EL1.FPEN disables it */
    uint64_t cpacr = READ_SYS(cpacr_el1);
    WRITE_SYS(cpacr_el1, 0);
    ISB();
    last_esr = ~0ULL;
    __asm__ volatile(".inst 0x1e622820" ::: "memory");          /* fadd d0, d1, d2 */
    CHECK_EQ(last_esr >> 26, 0x07);
    last_esr = ~0ULL;
    __asm__ volatile(".inst 0x4e228420" ::: "memory");          /* add v0.16b, v1.16b, v2.16b */
    CHECK_EQ(last_esr >> 26, 0x07);
    last_esr = ~0ULL;
    __asm__ volatile("mrs x0, s3_3_c4_c4_0" ::: "x0", "memory"); /* FPCR */
    CHECK_EQ(last_esr >> 26, 0x07);
    WRITE_SYS(cpacr_el1, 3 << 20);
    ISB();
    last_esr = 0;
    uint64_t dres;
    __asm__ volatile("fmov d1, #1.5\n fmov d2, #2.25\n fadd d0, d1, d2\n fmov %0, d0" : "=r"(dres) :: "v0", "v1", "v2", "memory");
    CHECK_EQ(last_esr, 0);
    CHECK_EQ(dres, 0x400E000000000000ULL);                     /* 3.75 */
    WRITE_SYS(cpacr_el1, cpacr);
    ISB();

    /* unknown system register traps */
    last_esr = ~0ULL;
    __asm__ volatile("mrs x0, s3_4_c15_c0_0" ::: "x0", "memory");
    CHECK_EQ(last_esr >> 26, 0);

    /* exception taken while using SP_EL0 goes to the EL1t vector */
    skip = 0;
    __asm__ volatile("mov x9, sp\n msr sp_el0, x9\n msr spsel, #0\n svc #1\n msr spsel, #1" ::: "x9", "memory");
    CHECK_EQ(last_kind, 0);

    /* alignment checking */
    static uint64_t buf[4];
    skip = 1;
    WRITE_SYS(sctlr_el1, READ_SYS(sctlr_el1) | 2);
    ISB();
    last_esr = 0;
    volatile uint64_t *p = (volatile uint64_t *)((uintptr_t)buf + 1);
    (void)*p;
    CHECK_EQ(last_esr >> 26, 0x25);
    CHECK_EQ(last_esr & 0x3F, 0x21);
    CHECK_EQ(last_far, (uintptr_t)p);
    WRITE_SYS(sctlr_el1, READ_SYS(sctlr_el1) & ~2ULL);
    ISB();
    last_esr = 0;
    (void)*p;                                                  /* allowed again */
    CHECK_EQ(last_esr, 0);

    /* misaligned PC */
    skip = 0;
    exc_hook = 0;
}

static void test_sysregs(void)
{
    CHECK_EQ(READ_SYS(CurrentEL), 4);
    CHECK_EQ(READ_SYS(mpidr_el1) & 0xFFFFFF, 0);
    CHECK_EQ((READ_SYS(id_aa64pfr0_el1) >> 16) & 0xFF, 0);     /* FP and AdvSIMD implemented */
    CHECK_EQ((READ_SYS(id_aa64isar0_el1) >> 20) & 0xF, 2);     /* LSE */
    CHECK_EQ((READ_SYS(id_aa64mmfr0_el1) >> 28) & 0xF, 0);     /* 4K granule */
    CHECK_EQ(READ_SYS(cntfrq_el0), 62500000);
    uint64_t t0 = READ_SYS(cntvct_el0);
    for (volatile int i = 0; i < 100; i++) {}
    CHECK(READ_SYS(cntvct_el0) > t0);
    WRITE_SYS(tpidr_el0, 0x1122334455667788ULL);
    CHECK_EQ(READ_SYS(tpidr_el0), 0x1122334455667788ULL);
    WRITE_SYS(tpidr_el1, 42);
    CHECK_EQ(READ_SYS(tpidr_el1), 42);
    uint64_t d = READ_SYS(daif);
    __asm__ volatile("msr daifclr, #0xf");
    CHECK_EQ(READ_SYS(daif), 0);
    __asm__ volatile("msr daifset, #2");
    CHECK_EQ(READ_SYS(daif), 0x80);
    WRITE_SYS(daif, d);
    CHECK_EQ(READ_SYS(dczid_el0) & 0xF, 4);

    /* DC ZVA clears a 64-byte block */
    static uint8_t blk[192] __attribute__((aligned(64)));
    memset(blk, 0xAA, sizeof blk);
    __asm__ volatile("dc zva, %0" :: "r"(blk + 64 + 13) : "memory");
    CHECK_EQ(blk[63], 0xAA);
    CHECK_EQ(blk[64], 0);
    CHECK_EQ(blk[127], 0);
    CHECK_EQ(blk[128], 0xAA);

    /* PSCI over HVC and SMC */
    CHECK_EQ(hvc(0x84000000, 0), 0x10000);
    CHECK_EQ(hvc(0x8400000A, 0x84000008), 0);
    CHECK_EQ((int64_t)hvc(0x8400000A, 0x12345), -1);
    CHECK_EQ((int64_t)hvc(0xC4000003, 0), -4);                  /* CPU_ON cpu0: already on */
    CHECK_EQ(hvc(0x84000004, 0), 0);
}

static void test_atomics(void)
{
    static uint64_t v64 __attribute__((aligned(16)));
    static uint32_t v32;
    uint64_t r, st;

    v64 = 5;
    __asm__ volatile("ldxr %0, [%2]\n add %0, %0, #1\n stxr %w1, %0, [%2]" : "=&r"(r), "=&r"(st) : "r"(&v64) : "memory");
    CHECK_EQ(st, 0);
    CHECK_EQ(v64, 6);
    __asm__ volatile("ldxr %0, [%2]\n clrex\n stxr %w1, %0, [%2]" : "=&r"(r), "=&r"(st) : "r"(&v64) : "memory");
    CHECK_EQ(st, 1);
    __asm__ volatile("mov %w1, #9\n stxr %w1, %0, [%2]" : "+r"(r), "=&r"(st) : "r"(&v64) : "memory");
    CHECK_EQ(st, 1);                                            /* no reservation */

    /* exclusive pair */
    static uint64_t pair[2] __attribute__((aligned(16))) = { 1, 2 };
    uint64_t a, b;
    __asm__ volatile("ldxp %0, %1, [%3]\n add %0, %0, #10\n add %1, %1, #20\n stxp %w2, %0, %1, [%3]"
                     : "=&r"(a), "=&r"(b), "=&r"(st) : "r"(pair) : "memory");
    CHECK_EQ(st, 0);
    CHECK_EQ(pair[0], 11);
    CHECK_EQ(pair[1], 22);

    /* LSE */
    v64 = 100;
    __asm__ volatile("ldadd %1, %0, [%2]" : "=r"(r) : "r"(7ULL), "r"(&v64) : "memory");
    CHECK_EQ(r, 100); CHECK_EQ(v64, 107);
    __asm__ volatile("swp %1, %0, [%2]" : "=r"(r) : "r"(0x55ULL), "r"(&v64) : "memory");
    CHECK_EQ(r, 107); CHECK_EQ(v64, 0x55);
    r = 0x55;
    __asm__ volatile("cas %0, %1, [%2]" : "+r"(r) : "r"(0x66ULL), "r"(&v64) : "memory");
    CHECK_EQ(r, 0x55); CHECK_EQ(v64, 0x66);
    r = 0x55;
    __asm__ volatile("casal %0, %1, [%2]" : "+r"(r) : "r"(0x77ULL), "r"(&v64) : "memory");
    CHECK_EQ(r, 0x66); CHECK_EQ(v64, 0x66);                     /* compare failed */
    v32 = 0xF0;
    uint32_t r32;
    __asm__ volatile("ldclr %w1, %w0, [%2]" : "=r"(r32) : "r"(0x30u), "r"(&v32) : "memory");
    CHECK_EQ(r32, 0xF0); CHECK_EQ(v32, 0xC0);
    __asm__ volatile("ldeor %w1, %w0, [%2]" : "=r"(r32) : "r"(0xFFu), "r"(&v32) : "memory");
    CHECK_EQ(v32, 0x3F);
    __asm__ volatile("ldset %w1, %w0, [%2]" : "=r"(r32) : "r"(0x100u), "r"(&v32) : "memory");
    CHECK_EQ(v32, 0x13F);
    v32 = (uint32_t)-5;
    __asm__ volatile("ldsmax %w1, %w0, [%2]" : "=r"(r32) : "r"(3u), "r"(&v32) : "memory");
    CHECK_EQ(v32, 3);
    __asm__ volatile("ldsmin %w1, %w0, [%2]" : "=r"(r32) : "r"((uint32_t)-9), "r"(&v32) : "memory");
    CHECK_EQ(v32, (uint32_t)-9);
    __asm__ volatile("ldumax %w1, %w0, [%2]" : "=r"(r32) : "r"(3u), "r"(&v32) : "memory");
    CHECK_EQ(v32, (uint32_t)-9);
    __asm__ volatile("ldumin %w1, %w0, [%2]" : "=r"(r32) : "r"(3u), "r"(&v32) : "memory");
    CHECK_EQ(v32, 3);
    uint8_t v8 = 250, r8;
    __asm__ volatile("ldaddb %w1, %w0, [%2]" : "=r"(r8) : "r"(10u), "r"(&v8) : "memory");
    CHECK_EQ(r8, 250); CHECK_EQ(v8, 4);                         /* wraps at 8 bits */

    /* CASP */
    static uint64_t cp[2] __attribute__((aligned(16))) = { 1, 2 };
    register uint64_t x0 __asm__("x0") = 1, x1 __asm__("x1") = 2, x2 __asm__("x2") = 30, x3 __asm__("x3") = 40;
    __asm__ volatile("casp x0, x1, x2, x3, [%4]" : "+r"(x0), "+r"(x1) : "r"(x2), "r"(x3), "r"(cp) : "memory");
    CHECK_EQ(x0, 1); CHECK_EQ(x1, 2);
    CHECK_EQ(cp[0], 30); CHECK_EQ(cp[1], 40);

    /* compiler generated atomics */
    static uint64_t counter;
    for (int i = 0; i < 1000; i++) __atomic_fetch_add(&counter, 3, __ATOMIC_SEQ_CST);
    CHECK_EQ(counter, 3000);
    uint64_t expected = 3000;
    CHECK(__atomic_compare_exchange_n(&counter, &expected, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    CHECK_EQ(counter, 1);

    /* load-acquire / store-release */
    __asm__ volatile("stlr %1, [%0]" :: "r"(&v64), "r"(0xABCULL) : "memory");
    __asm__ volatile("ldar %0, [%1]" : "=r"(r) : "r"(&v64) : "memory");
    CHECK_EQ(r, 0xABC);
    __asm__ volatile("ldapr %0, [%1]" : "=r"(r) : "r"(&v64) : "memory");
    CHECK_EQ(r, 0xABC);
}

static uint32_t smc_buf[16] __attribute__((aligned(64)));

/* code written at run time, then rewritten: the new instructions must run */
static void test_self_modifying(void)
{
    typedef uint64_t (*fn)(uint64_t);
    for (int round = 0; round < 3; round++) {
        smc_buf[0] = 0x91000000u | ((uint32_t)(1 + round) << 10);  /* add x0, x0, #(1 + round) */
        smc_buf[1] = 0xD65F03C0u;                                   /* ret */
        __asm__ volatile("dc cvau, %0\n dsb ish\n ic ivau, %0\n dsb ish\n isb" :: "r"(smc_buf) : "memory");
        uint64_t r = 0;
        for (int k = 0; k < 4; k++) r = ((fn)(uintptr_t)smc_buf)(r);
        CHECK_EQ(r, 4u * (1 + round));
    }
}

static void test_tbz(void)
{
    uint64_t hi = 1ULL << 40, lo = 1ULL << 8;
    int r1, r2, r3;
    __asm__ volatile("mov %w0, #0\n tbz %x1, #40, 1f\n mov %w0, #1\n1:" : "=&r"(r1) : "r"(hi));
    __asm__ volatile("mov %w0, #0\n tbnz %x1, #40, 1f\n mov %w0, #1\n1:" : "=&r"(r2) : "r"(lo));
    __asm__ volatile("mov %w0, #0\n tbnz %w1, #8, 1f\n mov %w0, #1\n1:" : "=&r"(r3) : "r"(lo));
    CHECK_EQ(r1, 1);
    CHECK_EQ(r2, 1);
    CHECK_EQ(r3, 0);
}

int main(void)
{
    test_tbz();
    test_self_modifying();
    test_exceptions();
    test_sysregs();
    test_atomics();
    printf_("t_sys: %d failures\n", test_failures);
    return test_failures ? 1 : 0;
}
