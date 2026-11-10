/*
 * Four CPUs: PSCI CPU_ON / AFFINITY_INFO / CPU_OFF, MPIDR, per-CPU
 * redistributors and timers, SGIs between CPUs, a spinlock built from
 * LDAXR/STLXR and LSE atomics, all on shared memory.
 * Run with -P 4.
 */
#include "rt.h"

#define NCPU  4
#define ITER  3000

extern char sec_entry[];
__asm__(".global sec_entry\n"
        "sec_entry:\n"
        "    mov x1, #(3 << 20)\n"
        "    msr cpacr_el1, x1\n"
        "    isb\n"
        "    mov sp, x0\n"                      /* context id = stack top */
        "    ldr x1, =vectors\n"
        "    msr vbar_el1, x1\n"
        "    isb\n"
        "    bl secondary_main\n"
        "1:  wfi\n"
        "    b 1b\n");

static uint8_t stacks[NCPU][16384] __attribute__((aligned(16)));
static volatile uint32_t lock;
static volatile uint64_t counter, atomic_counter;
static volatile uint64_t mpidr_seen[NCPU];
static volatile uint64_t xvar = 11;
static volatile int xstep, xfail;

/* CPU 1: a store by another CPU between LDXR and STXR must make the STXR fail */
static void exclusive_cpu1(void)
{
    uint64_t v;
    uint32_t st;
    __asm__ volatile("ldxr %0, [%1]" : "=r"(v) : "r"(&xvar) : "memory");
    xstep = 1;
    while (xstep != 2) __asm__ volatile("yield");
    __asm__ volatile("stxr %w0, %2, [%1]" : "=&r"(st) : "r"(&xvar), "r"(v + 1) : "memory");
    xfail = (int)st;
    do {                                                               /* and a normal retry loop then works */
        __asm__ volatile("ldxr %0, [%1]" : "=r"(v) : "r"(&xvar) : "memory");
        __asm__ volatile("stxr %w0, %2, [%1]" : "=&r"(st) : "r"(&xvar), "r"(v + 1) : "memory");
    } while (st);
    xstep = 3;
}

static volatile int started[NCPU], ticks[NCPU], sgi_from[NCPU], sgi6[NCPU], sgi7[NCPU], spi[NCPU], done[NCPU];
#define SPI_ID 40

static uint64_t hvc(uint64_t fn, uint64_t a, uint64_t b, uint64_t c)
{
    register uint64_t x0 __asm__("x0") = fn, x1 __asm__("x1") = a, x2 __asm__("x2") = b, x3 __asm__("x3") = c;
    __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}

static int cpu_id(void) { return (int)(READ_SYS(mpidr_el1) & 0xFF); }

static void spin_lock(void)
{
    uint32_t tmp, one = 1;
    __asm__ volatile("1: ldaxr %w0, [%1]\n"
                     "   cbnz %w0, 2f\n"
                     "   stxr %w0, %w2, [%1]\n"
                     "   cbnz %w0, 1b\n"
                     "   b 3f\n"
                     "2: wfe\n"
                     "   b 1b\n"
                     "3:\n" : "=&r"(tmp) : "r"(&lock), "r"(one) : "memory");
}

static void spin_unlock(void)
{
    __asm__ volatile("stlr wzr, [%0]\n sev" :: "r"(&lock) : "memory");
}

static int hook(struct frame *f, int kind)
{
    (void)f;
    if (kind != 5) return 0;
    uint64_t iar = READ_SYS(s3_0_c12_c12_0);
    uint32_t id = (uint32_t)iar & 0xFFFFFF;
    int me = cpu_id();
    if (id == 1023) return 1;
    if (id == 27) {
        ticks[me]++;
        WRITE_SYS(cntv_ctl_el0, 0);
    } else if (id >= 1 && id < NCPU && me == 0) {
        sgi_from[id]++;                                                /* CPU k signals with SGI k */
    } else if (id == 6) {
        sgi6[me]++;
    } else if (id == 7) {
        sgi7[me]++;
    } else if (id == SPI_ID) {
        spi[me]++;
    } else {
        test_failures++;
    }
    WRITE_SYS(s3_0_c12_c12_1, iar);
    return 1;
}

static void redist_init(int me)
{
    uintptr_t rd = GICR_BASE + 0x20000UL * (uintptr_t)me;
    CHECK_EQ(mmio_r64(rd + 0x8) >> 32, (uint64_t)me);                 /* GICR_TYPER affinity */
    CHECK_EQ((mmio_r64(rd + 0x8) >> 4) & 1, me == NCPU - 1);          /* Last */
    mmio_w32(rd + 0x14, mmio_r32(rd + 0x14) & ~2u);
    while (mmio_r32(rd + 0x14) & 4) {}
    mmio_w32(rd + 0x10000 + 0x80, 0xFFFFFFFFu);
    for (int i = 0; i < 32; i += 4) mmio_w32(rd + 0x10000 + 0x400 + i, 0x80808080u);
    mmio_w32(rd + 0x10000 + 0x100, (1u << 27) | 0xEEu);              /* timer, SGIs 1-3 and 5-7 */
    WRITE_SYS(s3_0_c12_c12_5, 7);
    ISB();
    WRITE_SYS(s3_0_c4_c6_0, 0xF0);
    WRITE_SYS(s3_0_c12_c12_7, 1);
    ISB();
}

static void work(void)
{
    for (int i = 0; i < ITER; i++) {
        spin_lock();
        counter = counter + 1;                                         /* plain read-modify-write under the lock */
        spin_unlock();
        uint64_t one = 1, old;
        __asm__ volatile("ldadd %1, %0, [%2]" : "=&r"(old) : "r"(one), "r"(&atomic_counter) : "memory");
    }
}

static void own_timer(int me)
{
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    WRITE_SYS(cntv_tval_el0, 5000 + 1000 * me);
    WRITE_SYS(cntv_ctl_el0, 1);
    while (!ticks[me]) __asm__ volatile("wfi");
}

void secondary_main(void)
{
    int me = cpu_id();
    mpidr_seen[me] = READ_SYS(mpidr_el1);
    started[me] = 1;
    if (me == 1) exclusive_cpu1();
    redist_init(me);
    own_timer(me);
    work();
    /* tell CPU 0, then wait for its answer */
    WRITE_SYS(s3_0_c12_c11_5, ((uint64_t)me << 24) | 1);              /* SGI <me> -> CPU 0 (target list) */
    done[me] = 1;
    while (!sgi6[me] || (me == 2 && !sgi7[me]) || (me == 3 && !spi[me])) __asm__ volatile("wfi");
    hvc(0x84000002, 0, 0, 0);                                          /* CPU_OFF: does not return */
    test_failures++;
}

int main(void)
{
    exc_hook = hook;
    CHECK_EQ(cpu_id(), 0);
    CHECK_EQ((mmio_r32(GICD_BASE + 0x4) >> 5) & 7, NCPU - 1);          /* GICD_TYPER.CPUNumber */
    mmio_w32(GICD_BASE + 0x0, 3);
    redist_init(0);
    for (int k = 1; k < NCPU; k++) {
        CHECK_EQ(hvc(0xC4000004, (uint64_t)k, 0, 0), 1);                /* AFFINITY_INFO: off */
        CHECK_EQ(hvc(0xC4000003, (uint64_t)k, (uint64_t)(uintptr_t)sec_entry,
                     (uint64_t)(uintptr_t)(stacks[k] + sizeof stacks[k])), 0);
        CHECK_EQ(hvc(0xC4000004, (uint64_t)k, 0, 0), 0);                /* on */
    }
    CHECK_EQ(hvc(0xC4000003, 1, (uint64_t)(uintptr_t)sec_entry, 0), (uint64_t)-4);   /* ALREADY_ON */
    CHECK_EQ(hvc(0xC4000003, NCPU, (uint64_t)(uintptr_t)sec_entry, 0), (uint64_t)-2);  /* no such CPU */
    while (xstep != 1) __asm__ volatile("yield");
    xvar = 55;
    xstep = 2;
    while (xstep != 3) __asm__ volatile("yield");
    CHECK_EQ(xfail, 1);
    CHECK_EQ(xvar, 56);
    own_timer(0);
    work();
    for (int k = 1; k < NCPU; k++) while (!done[k]) __asm__ volatile("wfe");
    for (int k = 1; k < NCPU; k++) CHECK_EQ(started[k], 1);
    /* the other CPUs' redistributors as seen (and changed) from CPU 0 */
    for (int k = 1; k < NCPU; k++)
        CHECK_EQ(mmio_r32(GICR_BASE + 0x20000UL * (uintptr_t)k + 0x10100), 0x080000EEu);
    mmio_w32(GICR_BASE + 0x20000UL * 1 + 0x10180, 1u << 7);                      /* ICENABLER0: SGI 7 off on CPU 1 */
    CHECK_EQ(mmio_r32(GICR_BASE + 0x20000UL * 1 + 0x10100), 0x0800006Eu);
    CHECK_EQ(mmio_r32(GICR_BASE + 0x10100), 0x080000EEu);
    for (int k = 1; k < NCPU; k++) while (!sgi_from[k]) __asm__ volatile("wfi");
    WRITE_SYS(s3_0_c12_c11_5, (7ULL << 24) | (1ULL << 2));             /* SGI 7 -> CPU 2 only */
    /* a shared peripheral interrupt routed to CPU 3 (GICD_IROUTER Aff0 = 3), made pending by software */
    mmio_w64(GICD_BASE + 0x6000 + 8 * SPI_ID, 3);
    CHECK_EQ(mmio_r64(GICD_BASE + 0x6000 + 8 * SPI_ID), 3);
    mmio_w32(GICD_BASE + 0x400 + (SPI_ID & ~3), 0x80808080u);
    mmio_w32(GICD_BASE + 0x80 + 4 * (SPI_ID / 32), 0xFFFFFFFFu);
    mmio_w32(GICD_BASE + 0x100 + 4 * (SPI_ID / 32), 1u << (SPI_ID % 32));
    mmio_w32(GICD_BASE + 0x200 + 4 * (SPI_ID / 32), 1u << (SPI_ID % 32));
    WRITE_SYS(s3_0_c12_c11_5, (6ULL << 24) | (1ULL << 40));            /* SGI 6 -> all but self */
    for (int k = 1; k < NCPU; k++) while (hvc(0xC4000004, (uint64_t)k, 0, 0) != 1) __asm__ volatile("wfe");
    for (int k = 1; k < NCPU; k++) {
        CHECK_EQ(mpidr_seen[k] & 0xFF, (uint64_t)k);
        CHECK_EQ(ticks[k], 1);
        CHECK_EQ(sgi6[k], 1);
        CHECK_EQ(sgi7[k], k == 2);
        CHECK_EQ(sgi_from[k], 1);
        CHECK_EQ(spi[k], k == 3);
    }
    CHECK_EQ(ticks[0], 1);
    CHECK_EQ(sgi6[0], 0);
    CHECK_EQ(sgi7[0], 0);
    CHECK_EQ(spi[0], 0);
    CHECK_EQ(counter, (uint64_t)NCPU * ITER);
    CHECK_EQ(atomic_counter, (uint64_t)NCPU * ITER);
    printf_("smp: %d CPUs, counter=%llu atomic=%llu, %d failures\n", NCPU, (unsigned long long)counter,
            (unsigned long long)atomic_counter, test_failures);
    return test_failures ? 1 : 0;
}
