/* EL0: entering user mode, syscalls, traps from EL0, PAN and unprivileged loads. */
#include "rt.h"

#define PG 4096
#define AF (1ULL << 10)
#define NORMAL (AF | (3ULL << 8) | (1ULL << 2))
#define AP_EL0RW (1ULL << 6)
#define PXN (1ULL << 53)
#define UXN (1ULL << 54)

#define U_CODE  0x80000000ULL
#define U_STACK 0x80001000ULL
#define U_KONLY 0x80002000ULL

static uint64_t l1[512] __attribute__((aligned(PG)));
static uint64_t l2[512] __attribute__((aligned(PG)));
static uint64_t l3[512] __attribute__((aligned(PG)));
static uint8_t ucode[PG] __attribute__((aligned(PG)));
static uint8_t ustack[PG] __attribute__((aligned(PG)));
static uint8_t konly[PG] __attribute__((aligned(PG)));

extern char user_start[], user_end[];
extern uint64_t enter_user(uint64_t entry, uint64_t sp);
extern char user_return[];

__asm__(
    "    .text\n"
    "    .balign 4\n"
    "user_start:\n"
    "    mov x8, #1\n"
    "    mov x0, #'U'\n"
    "    svc #0\n"                                  /* write 'U' */
    "    mrs x1, sctlr_el1\n"                       /* undefined at EL0 */
    "    movz x2, #0x8000, lsl #16\n"
    "    movk x2, #0x2000\n"
    "    ldr x3, [x2]\n"                            /* EL1-only page: permission fault */
    "    stp x29, x30, [sp, #-16]!\n"               /* user stack works */
    "    ldp x29, x30, [sp], #16\n"
    "    mrs x4, tpidrro_el0\n"
    "    msr tpidrro_el0, x4\n"                     /* read-only at EL0: undefined */
    "    mrs x5, cntvct_el0\n"                      /* allowed through CNTKCTL_EL1 */
    "    dc zva, x2\n"                              /* SCTLR_EL1.DZE = 0: undefined */
    "    wfi\n"                                     /* SCTLR_EL1.nTWI = 0: trapped */
    "    mov x8, #0\n"
    "    add x0, x4, #1\n"
    "    svc #0\n"                                  /* exit */
    "1:  b 1b\n"
    "user_end:\n"
    "\n"
    "    .global enter_user\n"
    "enter_user:\n"
    "    stp x29, x30, [sp, #-96]!\n"
    "    stp x19, x20, [sp, #16]\n"
    "    stp x21, x22, [sp, #32]\n"
    "    stp x23, x24, [sp, #48]\n"
    "    stp x25, x26, [sp, #64]\n"
    "    stp x27, x28, [sp, #80]\n"
    "    msr sp_el0, x1\n"
    "    msr elr_el1, x0\n"
    "    msr spsr_el1, xzr\n"
    "    eret\n"
    "    .global user_return\n"
    "user_return:\n"
    "    ldp x19, x20, [sp, #16]\n"
    "    ldp x21, x22, [sp, #32]\n"
    "    ldp x23, x24, [sp, #48]\n"
    "    ldp x25, x26, [sp, #64]\n"
    "    ldp x27, x28, [sp, #80]\n"
    "    ldp x29, x30, [sp], #96\n"
    "    adrp x0, exit_value\n"
    "    ldr x0, [x0, :lo12:exit_value]\n"
    "    ret\n");

uint64_t exit_value;
static char out[16];
static volatile int nout, n_undef, n_dabt, n_wfi, n_other;
static volatile uint64_t dabt_esr, dabt_far, user_sp_seen;

static int hook(struct frame *f, int kind)
{
    uint64_t esr = READ_SYS(esr_el1), ec = esr >> 26;
    if (kind == 8) {                                  /* synchronous from EL0 */
        if (ec == 0x15) {
            if (f->x[8] == 1) {
                if (nout < 15) out[nout++] = (char)f->x[0];
                return 1;
            }
            exit_value = f->x[0];
            user_sp_seen = f->sp_el0;
            f->elr = (uintptr_t)user_return;
            f->spsr = 0x3C5;                          /* EL1h, DAIF masked */
            return 1;
        }
        if (ec == 0x00) { n_undef++; f->elr += 4; return 1; }
        if (ec == 0x24) { n_dabt++; dabt_esr = esr; dabt_far = READ_SYS(far_el1); f->elr += 4; return 1; }
        if (ec == 0x01) { n_wfi++; f->elr += 4; return 1; }
        n_other++;
        f->elr += 4;
        return 1;
    }
    if (kind == 4 && ec == 0x25) {                     /* EL1 data abort (PAN / LDTR tests) */
        dabt_esr = esr;
        n_dabt++;
        f->elr += 4;
        return 1;
    }
    return 0;
}

int main(void)
{
    exc_hook = hook;

    l1[1] = 0x40000000ULL | 1 | NORMAL;                         /* kernel identity, EL1 only */
    l1[0] = 0x00000000ULL | 1 | AF | PXN | UXN;                 /* devices */
    l1[2] = (uintptr_t)l2 | 3;
    l2[0] = (uintptr_t)l3 | 3;
    l3[0] = (uintptr_t)ucode | 3 | NORMAL | AP_EL0RW | PXN;     /* user code */
    l3[1] = (uintptr_t)ustack | 3 | NORMAL | AP_EL0RW | PXN | UXN;
    l3[2] = (uintptr_t)konly | 3 | NORMAL | PXN | UXN;          /* EL1 only */
    memcpy(ucode, user_start, (size_t)(user_end - user_start));
    __asm__ volatile("dsb ish\n ic iallu\n dsb ish\n isb" ::: "memory");

    WRITE_SYS(mair_el1, 0xFF00ULL);
    WRITE_SYS(tcr_el1, 25 | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (1ULL << 23) | (2ULL << 32));
    WRITE_SYS(ttbr0_el1, (uintptr_t)l1);
    __asm__ volatile("tlbi vmalle1\n dsb ish\n isb" ::: "memory");
    WRITE_SYS(sctlr_el1, (READ_SYS(sctlr_el1) | 1 | (1 << 23)) & ~((1ULL << 16) | (1ULL << 14)));
    ISB();

    WRITE_SYS(tpidrro_el0, 0x777);
    WRITE_SYS(cntkctl_el1, 2);                                  /* EL0VCTEN */

    uint64_t r = enter_user(U_CODE, U_STACK + PG);
    CHECK_EQ(r, 0x778);
    CHECK_EQ(nout, 1);
    CHECK_EQ(out[0], 'U');
    CHECK_EQ(n_undef, 3);
    CHECK_EQ(n_dabt, 1);
    CHECK_EQ(dabt_esr >> 26, 0x24);
    CHECK_EQ(dabt_esr & 0x3F, 0x0F);
    CHECK_EQ(dabt_far, U_KONLY);
    CHECK_EQ(n_wfi, 1);
    CHECK_EQ(n_other, 0);
    CHECK_EQ(user_sp_seen, U_STACK + PG);
    CHECK_EQ(READ_SYS(CurrentEL), 4);

    /* PAN: EL1 data accesses to EL0-accessible pages fault */
    volatile uint64_t *up = (volatile uint64_t *)U_STACK;
    *up = 123;                                                  /* PAN off: fine */
    __asm__ volatile("msr pan, #1" ::: "memory");
    CHECK_EQ(READ_SYS(s3_0_c4_c2_3) >> 22, 1);
    n_dabt = 0;
    (void)*up;
    CHECK_EQ(n_dabt, 1);
    CHECK_EQ(dabt_esr & 0x3F, 0x0F);
    uint64_t v;
    __asm__ volatile("ldtr %0, [%1]" : "=r"(v) : "r"(U_STACK) : "memory");   /* unprivileged load ok */
    CHECK_EQ(v, 123);
    __asm__ volatile("msr pan, #0" ::: "memory");

    /* LDTR/STTR use EL0 permissions */
    n_dabt = 0;
    __asm__ volatile("sttr %0, [%1]" :: "r"(5ULL), "r"(U_KONLY) : "memory");
    CHECK_EQ(n_dabt, 1);
    __asm__ volatile("sttr %0, [%1]" :: "r"(9ULL), "r"(U_STACK + 8) : "memory");
    CHECK_EQ(((volatile uint64_t *)ustack)[1], 9);

    printf_("t_el0: %d failures\n", test_failures);
    return test_failures ? 1 : 0;
}
