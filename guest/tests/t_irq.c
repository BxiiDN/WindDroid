/* GICv3 distributor/redistributor/CPU interface, generic timer, SGIs and UART RX IRQs.
 * The test runner feeds "hey!" to the UART before starting. */
#include "rt.h"

#define TICK 20000

static volatile int ticks, sgis, spurious, uart_irqs, other;
static volatile uint64_t rpr_in_handler, last_tick_count;
static char rx[16];
static volatile int nrx;

static int hook(struct frame *f, int kind)
{
    (void)f;
    if (kind != 5) return 0;                       /* IRQ, current EL with SP_ELx */
    uint64_t iar = READ_SYS(s3_0_c12_c12_0);       /* ICC_IAR1_EL1 */
    uint32_t id = (uint32_t)iar & 0xFFFFFF;
    rpr_in_handler = READ_SYS(s3_0_c12_c11_3);     /* ICC_RPR_EL1 */
    if (id == 27) {
        ticks++;
        last_tick_count = READ_SYS(cntvct_el0);
        WRITE_SYS(cntv_tval_el0, TICK);
    } else if (id == 3) {
        sgis++;
    } else if (id == 33) {
        uart_irqs++;
        int c;
        while ((c = getc_nb()) >= 0) if (nrx < 15) rx[nrx++] = (char)c;
    } else if (id == 1023) {
        spurious++;
        return 1;
    } else {
        other++;
    }
    WRITE_SYS(s3_0_c12_c12_1, iar);                /* ICC_EOIR1_EL1 */
    return 1;
}

static void gic_init(void)
{
    /* redistributor: wake up */
    mmio_w32(GICR_BASE + 0x14, mmio_r32(GICR_BASE + 0x14) & ~2u);
    while (mmio_r32(GICR_BASE + 0x14) & 4) {}
    /* SGIs/PPIs: group 1, priority 0x80, enable 3 and 27 */
    mmio_w32(GICR_BASE + 0x10000 + 0x80, 0xFFFFFFFFu);
    for (int i = 0; i < 32; i += 4) mmio_w32(GICR_BASE + 0x10000 + 0x400 + i, 0x80808080u);
    mmio_w32(GICR_BASE + 0x10000 + 0x100, (1u << 27) | (1u << 3));
    /* distributor: enable, SPI 33 (UART) group 1, priority 0xA0 */
    mmio_w32(GICD_BASE + 0x0, 3);
    mmio_w32(GICD_BASE + 0x80 + 4, 0xFFFFFFFFu);
    *(volatile uint8_t *)(GICD_BASE + 0x400 + 33) = 0xA0;
    mmio_w64(GICD_BASE + 0x6000 + 33 * 8, 0);
    mmio_w32(GICD_BASE + 0x100 + 4, 1u << 1);
    /* CPU interface */
    WRITE_SYS(s3_0_c12_c12_5, 7);                  /* ICC_SRE_EL1 */
    ISB();
    WRITE_SYS(s3_0_c4_c6_0, 0xF0);                 /* ICC_PMR_EL1 */
    WRITE_SYS(s3_0_c12_c12_7, 1);                  /* ICC_IGRPEN1_EL1 */
    ISB();
}

int main(void)
{
    exc_hook = hook;

    /* register level checks */
    CHECK_EQ((mmio_r32(GICD_BASE + 0xFFE8) >> 4) & 0xF, 3);
    CHECK_EQ(mmio_r32(GICD_BASE + 0x4) & 0x1F, 2);
    CHECK_EQ(mmio_r64(GICR_BASE + 0x8) & 0x10, 0x10);       /* GICR_TYPER.Last */
    gic_init();
    CHECK_EQ(mmio_r32(GICD_BASE + 0x100 + 4), 2);
    CHECK_EQ(*(volatile uint8_t *)(GICD_BASE + 0x400 + 33), 0xA0);
    CHECK_EQ(READ_SYS(s3_0_c4_c6_0), 0xF0);

    /* 1. timer interrupts while waiting in WFI */
    WRITE_SYS(cntv_tval_el0, TICK);
    WRITE_SYS(cntv_ctl_el0, 1);
    uint64_t t0 = READ_SYS(cntvct_el0);
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    while (ticks < 5) __asm__ volatile("wfi");
    __asm__ volatile("msr daifset, #2" ::: "memory");
    uint64_t elapsed = last_tick_count - t0;
    CHECK(elapsed >= 5 * TICK);
    CHECK(elapsed < 5 * TICK + 5000);
    CHECK_EQ(rpr_in_handler, 0x80);
    CHECK_EQ(READ_SYS(s3_0_c12_c11_3), 0xFF);       /* idle running priority */
    WRITE_SYS(cntv_ctl_el0, 0);

    /* 2. timer status bits */
    WRITE_SYS(cntv_cval_el0, 0);
    WRITE_SYS(cntv_ctl_el0, 3);                     /* enabled, masked */
    CHECK_EQ(READ_SYS(cntv_ctl_el0), 7);            /* ISTATUS set */
    WRITE_SYS(cntv_ctl_el0, 0);

    /* 3. software generated interrupt */
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    WRITE_SYS(s3_0_c12_c11_5, (3ULL << 24) | 1);    /* ICC_SGI1R_EL1: INTID 3 -> CPU 0 */
    ISB();
    for (volatile int i = 0; i < 100; i++) {}
    CHECK_EQ(sgis, 1);

    /* 4. priority mask blocks delivery until lowered */
    WRITE_SYS(s3_0_c4_c6_0, 0x10);
    WRITE_SYS(s3_0_c12_c11_5, (3ULL << 24) | 1);
    for (volatile int i = 0; i < 100; i++) {}
    CHECK_EQ(sgis, 1);
    CHECK_EQ(READ_SYS(s3_0_c12_c8_2) & 0xFFFFFF, 3);   /* ICC_HPPIR1 still shows it */
    WRITE_SYS(s3_0_c4_c6_0, 0xF0);
    ISB();
    for (volatile int i = 0; i < 100; i++) {}
    CHECK_EQ(sgis, 2);

    /* 5. UART receive interrupt (input injected by the test runner) */
    mmio_w32(UART_BASE + 0x38, (1u << 4) | (1u << 6));
    while (uart_irqs == 0) __asm__ volatile("wfi");
    rx[nrx] = 0;
    CHECK_EQ(strcmp(rx, "hey!"), 0);
    CHECK_EQ(mmio_r32(UART_BASE + 0x40), 0);         /* nothing left pending */
    __asm__ volatile("msr daifset, #2" ::: "memory");

    /* 6. reading IAR with nothing pending returns spurious */
    CHECK_EQ(READ_SYS(s3_0_c12_c12_0) & 0xFFFFFF, 1023);
    CHECK_EQ(other, 0);

    printf_("t_irq: ticks=%d sgis=%d uart=%d rx=\"%s\" failures=%d\n", ticks, sgis, uart_irqs, rx, test_failures);
    return test_failures ? 1 : 0;
}
