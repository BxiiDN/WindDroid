/* WindDroid guest runtime: entry point and exception vectors. */
    .section .text.boot, "ax"
    .global _start
_start:
    mov     x0, #(3 << 20)          /* CPACR_EL1.FPEN: no FP/SIMD traps */
    msr     cpacr_el1, x0
    isb
    ldr     x0, =__stack_top
    mov     sp, x0
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:  ldr     x0, =vectors
    msr     vbar_el1, x0
    isb
    bl      main
    bl      wd_exit
3:  b       3b

/* Every vector saves the full register frame and calls
 * void exc_handler(struct frame *f, int kind)   kind = vector index 0..15 */
    .macro VEC kind
    .balign 0x80
    sub     sp, sp, #(272 + 528)
    stp     x0, x1, [sp, #0]
    mov     x1, #\kind
    b       vec_common
    .endm

    .balign 0x800
    .global vectors
vectors:
    VEC 0
    VEC 1
    VEC 2
    VEC 3
    VEC 4
    VEC 5
    VEC 6
    VEC 7
    VEC 8
    VEC 9
    VEC 10
    VEC 11
    VEC 12
    VEC 13
    VEC 14
    VEC 15

vec_common:
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    mrs     x2, elr_el1
    mrs     x3, spsr_el1
    stp     x30, x2, [sp, #240]
    mrs     x2, sp_el0
    stp     x3, x2, [sp, #256]
#ifndef NO_FP_SAVE
    mrs     x2, cpacr_el1
    tbz     x2, #20, 7f             /* FP disabled: nothing to save */
    add     x2, sp, #272
    stp     q0, q1, [x2, #0]
    stp     q2, q3, [x2, #32]
    stp     q4, q5, [x2, #64]
    stp     q6, q7, [x2, #96]
    stp     q8, q9, [x2, #128]
    stp     q10, q11, [x2, #160]
    stp     q12, q13, [x2, #192]
    stp     q14, q15, [x2, #224]
    stp     q16, q17, [x2, #256]
    stp     q18, q19, [x2, #288]
    stp     q20, q21, [x2, #320]
    stp     q22, q23, [x2, #352]
    stp     q24, q25, [x2, #384]
    stp     q26, q27, [x2, #416]
    stp     q28, q29, [x2, #448]
    stp     q30, q31, [x2, #480]
    mrs     x3, fpcr
    mrs     x4, fpsr
    str     x3, [x2, #512]
    str     x4, [x2, #520]
7:
#endif
    mov     x0, sp
    bl      exc_handler
#ifndef NO_FP_SAVE
    mrs     x2, cpacr_el1
    tbz     x2, #20, 8f
    add     x2, sp, #272
    ldp     q0, q1, [x2, #0]
    ldp     q2, q3, [x2, #32]
    ldp     q4, q5, [x2, #64]
    ldp     q6, q7, [x2, #96]
    ldp     q8, q9, [x2, #128]
    ldp     q10, q11, [x2, #160]
    ldp     q12, q13, [x2, #192]
    ldp     q14, q15, [x2, #224]
    ldp     q16, q17, [x2, #256]
    ldp     q18, q19, [x2, #288]
    ldp     q20, q21, [x2, #320]
    ldp     q22, q23, [x2, #352]
    ldp     q24, q25, [x2, #384]
    ldp     q26, q27, [x2, #416]
    ldp     q28, q29, [x2, #448]
    ldp     q30, q31, [x2, #480]
    ldr     x3, [x2, #512]
    ldr     x4, [x2, #520]
    msr     fpcr, x3
    msr     fpsr, x4
8:
#endif
    ldp     x3, x2, [sp, #256]
    msr     sp_el0, x2
    ldp     x30, x2, [sp, #240]
    msr     elr_el1, x2
    msr     spsr_el1, x3
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldp     x0, x1, [sp, #0]
    add     sp, sp, #(272 + 528)
    eret
    .ltorg
