/* Stage 1 MMU: page tables, faults, AT, TLB maintenance, execute permissions. */
#include "rt.h"

#define PG 4096
#define DESC_VALID  1ULL
#define DESC_TABLE  3ULL
#define DESC_PAGE   3ULL
#define DESC_BLOCK  1ULL
#define AF          (1ULL << 10)
#define SH_INNER    (3ULL << 8)
#define ATTR(n)     ((uint64_t)(n) << 2)
#define AP_RO       (2ULL << 6)
#define AP_EL0RW    (1ULL << 6)
#define PXN         (1ULL << 53)
#define UXN         (1ULL << 54)
#define NORMAL      (AF | SH_INNER | ATTR(1))

#define HI_BASE 0xFFFFFFC000000000ULL   /* l1_hi[256] -> l2_hi, l2_hi[0] -> l3_hi */

static uint64_t l1_lo[512] __attribute__((aligned(PG)));
static uint64_t l1_hi[512] __attribute__((aligned(PG)));
static uint64_t l2_hi[512] __attribute__((aligned(PG)));
static uint64_t l3_hi[512] __attribute__((aligned(PG)));
static uint8_t pg0[PG] __attribute__((aligned(PG)));
static uint8_t pg1[PG] __attribute__((aligned(PG)));
static uint32_t pg2[PG / 4] __attribute__((aligned(PG)));

static volatile uint64_t f_esr, f_far;
static volatile int faults;

static int hook(struct frame *f, int kind)
{
    (void)kind;
    f_esr = READ_SYS(esr_el1);
    f_far = READ_SYS(far_el1);
    faults++;
    uint64_t ec = f_esr >> 26;
    if (ec == 0x21) f->elr = f->x[30];          /* instruction abort in a called function: return */
    else f->elr += 4;
    return 1;
}

static void tlbi(void)
{
    __asm__ volatile("dsb ishst\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
}

static void mmu_on(void)
{
    l1_lo[0] = 0x00000000ULL | DESC_BLOCK | AF | ATTR(0) | PXN | UXN;   /* devices */
    l1_lo[1] = 0x40000000ULL | DESC_BLOCK | NORMAL;                      /* RAM, identity */
    l1_hi[256] = (uintptr_t)l2_hi | DESC_TABLE;
    l2_hi[0] = (uintptr_t)l3_hi | DESC_TABLE;

    WRITE_SYS(mair_el1, 0xFF00ULL);
    uint64_t tcr = 25 | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) |             /* T0SZ, cacheable, inner */
                   (25ULL << 16) | (1ULL << 24) | (1ULL << 26) | (3ULL << 28) |  /* T1SZ ... */
                   (2ULL << 30) | (2ULL << 32);                                  /* TG1=4K, IPS=40 bit */
    WRITE_SYS(tcr_el1, tcr);
    WRITE_SYS(ttbr0_el1, (uintptr_t)l1_lo);
    WRITE_SYS(ttbr1_el1, (uintptr_t)l1_hi);
    tlbi();
    WRITE_SYS(sctlr_el1, READ_SYS(sctlr_el1) | 1 | (1 << 2) | (1 << 12));
    ISB();
}

static uint64_t at_s1e1r(uint64_t va)
{
    __asm__ volatile("at s1e1r, %0\n isb" :: "r"(va) : "memory");
    return READ_SYS(par_el1);
}

int main(void)
{
    exc_hook = hook;
    mmu_on();
    CHECK(READ_SYS(sctlr_el1) & 1);
    puts_("t_mmu: MMU enabled, still running from identity map\n");

    volatile uint64_t *hv;

    /* 1. high VA alias of pg0 */
    l3_hi[0] = (uintptr_t)pg0 | DESC_PAGE | NORMAL;
    hv = (volatile uint64_t *)(HI_BASE + 0 * PG);
    *hv = 0xDEADBEEFCAFEF00DULL;
    CHECK_EQ(*(volatile uint64_t *)pg0, 0xDEADBEEFCAFEF00DULL);
    hv[100] = 7;
    CHECK_EQ(((volatile uint64_t *)pg0)[100], 7);

    /* 2. read-only alias: reads ok, writes take a permission fault */
    l3_hi[1] = (uintptr_t)pg0 | DESC_PAGE | NORMAL | AP_RO;
    hv = (volatile uint64_t *)(HI_BASE + 1 * PG);
    CHECK_EQ(*hv, 0xDEADBEEFCAFEF00DULL);
    faults = 0;
    *hv = 1;
    CHECK_EQ(faults, 1);
    CHECK_EQ(f_esr >> 26, 0x25);
    CHECK_EQ(f_esr & 0x3F, 0x0F);                 /* permission fault, level 3 */
    CHECK_EQ((f_esr >> 6) & 1, 1);                /* WnR */
    CHECK_EQ(f_far, HI_BASE + 1 * PG);
    CHECK_EQ(*(volatile uint64_t *)pg0, 0xDEADBEEFCAFEF00DULL);

    /* 3. invalid page: translation fault level 3 */
    faults = 0;
    hv = (volatile uint64_t *)(HI_BASE + 2 * PG + 8);
    (void)*hv;
    CHECK_EQ(faults, 1);
    CHECK_EQ(f_esr & 0x3F, 0x07);
    CHECK_EQ((f_esr >> 6) & 1, 0);
    CHECK_EQ(f_far, HI_BASE + 2 * PG + 8);

    /* 4. access flag clear */
    l3_hi[3] = (uintptr_t)pg0 | DESC_PAGE | (NORMAL & ~AF);
    faults = 0;
    (void)*(volatile uint64_t *)(HI_BASE + 3 * PG);
    CHECK_EQ(f_esr & 0x3F, 0x0B);

    /* 5. missing level 1 entry */
    faults = 0;
    (void)*(volatile uint64_t *)0xFFFFFF8000001000ULL;
    CHECK_EQ(faults, 1);
    CHECK_EQ(f_esr & 0x3F, 0x05);

    /* 6. address outside the TTBR0 range */
    faults = 0;
    (void)*(volatile uint64_t *)0x0000800000000000ULL;
    CHECK_EQ(faults, 1);
    CHECK_EQ(f_esr & 0x3F, 0x04);

    /* 7. address translation instructions */
    uint64_t par = at_s1e1r(HI_BASE + 0 * PG + 0x123);
    CHECK_EQ(par & 1, 0);
    CHECK_EQ(par & 0x0000FFFFFFFFF000ULL, (uintptr_t)pg0);
    CHECK_EQ(par >> 56, 0xFF);                    /* MAIR attribute */
    par = at_s1e1r(HI_BASE + 2 * PG);
    CHECK_EQ(par & 1, 1);
    CHECK_EQ((par >> 1) & 0x3F, 0x07);

    /* 8. remap + TLB invalidation */
    memset(pg1, 0x5A, PG);
    l3_hi[0] = (uintptr_t)pg1 | DESC_PAGE | NORMAL;
    tlbi();
    CHECK_EQ(*(volatile uint64_t *)(HI_BASE), 0x5A5A5A5A5A5A5A5AULL);

    /* 9. execute from a high VA, then from a PXN page */
    pg2[0] = 0x52800540;                          /* mov w0, #42 */
    pg2[1] = 0xD65F03C0;                          /* ret */
    __asm__ volatile("dsb ish\n ic iallu\n dsb ish\n isb" ::: "memory");
    l3_hi[4] = (uintptr_t)pg2 | DESC_PAGE | NORMAL | AP_RO;
    l3_hi[5] = (uintptr_t)pg2 | DESC_PAGE | NORMAL | AP_RO | PXN;
    tlbi();
    int (*fn)(void) = (int (*)(void))(HI_BASE + 4 * PG);
    CHECK_EQ(fn(), 42);
    faults = 0;
    fn = (int (*)(void))(HI_BASE + 5 * PG);
    fn();
    CHECK_EQ(faults, 1);
    CHECK_EQ(f_esr >> 26, 0x21);
    CHECK_EQ(f_esr & 0x3F, 0x0F);
    CHECK_EQ(f_far, HI_BASE + 5 * PG);

    /* 10. 2 MiB block at level 2 mapping the start of RAM */
    l2_hi[1] = 0x40000000ULL | DESC_BLOCK | NORMAL;
    tlbi();
    extern char _start[];
    uint64_t off = (uintptr_t)_start - 0x40000000ULL;
    CHECK_EQ(*(volatile uint32_t *)(HI_BASE + 0x200000 + off), *(volatile uint32_t *)_start);
    par = at_s1e1r(HI_BASE + 0x200000 + 0x1234);
    CHECK_EQ(par & 0x0000FFFFFFFFF000ULL, 0x40001000ULL);

    /* 11. UXN only restricts EL0; EL1 may still execute */
    l3_hi[6] = (uintptr_t)pg2 | DESC_PAGE | NORMAL | AP_RO | UXN;
    tlbi();
    faults = 0;
    fn = (int (*)(void))(HI_BASE + 6 * PG);
    CHECK_EQ(fn(), 42);
    CHECK_EQ(faults, 0);

    printf_("t_mmu: %d failures\n", test_failures);
    return test_failures ? 1 : 0;
}
