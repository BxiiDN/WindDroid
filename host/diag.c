/*
 * WindDroid - diagnostic report: where the virtual CPUs spend their time
 * (sampled program counters with the code around them), recent exceptions,
 * CPU and device state. Meant to be copied from the app and sent to the
 * developer when a guest hangs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>

#include "../core/internal.h"

typedef struct { uint64_t pc; uint32_t count; uint8_t cpu; } hot;

static int by_count(const void *a, const void *b)
{
    const hot *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

#define OUT(...) do { if (n >= 0 && (size_t)n < len) n += snprintf(buf + n, len - (size_t)n, __VA_ARGS__); } while (0)

static void cpu_line(char *buf, size_t len, int *pn, wd_machine *m, int k, const wd_cpu *c)
{
    int n = *pn;
    OUT("cpu%d %s%s pc=%016llx EL%d sp0=%llx sp1=%llx daif=%x\n", k, m->cpu_on[k] ? "on" : "off",
        (k == m->cur ? m->wfi : m->cpu_wfi[k]) ? " wfi" : "", (unsigned long long)c->pc, c->el,
        (unsigned long long)c->sp_el0, (unsigned long long)c->sp_el1, c->daif >> 6);
    OUT("     elr=%016llx esr=%llx far=%llx spsr=%llx vbar=%llx\n", (unsigned long long)c->elr,
        (unsigned long long)c->esr, (unsigned long long)c->far, (unsigned long long)c->spsr, (unsigned long long)c->vbar);
    OUT("     sctlr=%llx tcr=%llx ttbr0=%llx ttbr1=%llx\n", (unsigned long long)c->sctlr, (unsigned long long)c->tcr,
        (unsigned long long)c->ttbr0, (unsigned long long)c->ttbr1);
    OUT("     x0=%llx x1=%llx x2=%llx x3=%llx x8=%llx x19=%llx x29=%llx x30=%llx\n", (unsigned long long)c->x[0],
        (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3],
        (unsigned long long)c->x[8], (unsigned long long)c->x[19], (unsigned long long)c->x[29], (unsigned long long)c->x[30]);
    *pn = n;
}

size_t wd_diag_report(wd_machine *m, char *buf, size_t len)
{
    int n = 0;
    if (!len) return 0;
    buf[0] = 0;
    OUT("WindDroid %s diagnostics: %d CPU, RAM %llu MB, %llu instructions, %llu interrupts, jit=%d\n", WD_VERSION,
        m->ncpu, (unsigned long long)(m->ram_size >> 20), (unsigned long long)m->cpu.icount,
        (unsigned long long)m->diag_irqs, m->use_jit);
    for (int k = 0; k < m->ncpu; k++) cpu_line(buf, len, &n, m, k, k == m->cur ? &m->cpu : &m->cpus[k]);

    /* hottest sampled program counters */
    uint32_t ns = m->diag_n < WD_DIAG_SAMPLES ? m->diag_n : WD_DIAG_SAMPLES;
    hot *h = calloc(ns ? ns : 1, sizeof *h);
    int nh = 0;
    for (uint32_t i = 0; h && i < ns; i++) {
        uint64_t pc = m->diag_pc[i] & ~0xFULL;             /* 16-byte buckets */
        int j = 0;
        while (j < nh && (h[j].pc != pc || h[j].cpu != m->diag_cpu[i])) j++;
        if (j == nh) { if (nh >= 512) continue; h[nh].pc = pc; h[nh].cpu = m->diag_cpu[i]; h[nh].count = 0; nh++; }
        h[j].count++;
    }
    if (h) qsort(h, (size_t)nh, sizeof *h, by_count);
    OUT("hot code (%u samples):\n", ns);
    for (int j = 0; j < nh && j < 8; j++)
        OUT("  %3u%% cpu%d %016llx\n", ns ? h[j].count * 100 / ns : 0, h[j].cpu, (unsigned long long)h[j].pc);
    for (int j = 0; j < nh && j < 3; j++) {
        OUT("  code at %016llx:", (unsigned long long)h[j].pc - 32);
        for (int w = 0; w < 24; w++) {
            uint32_t insn;
            if (wd_peek_insn(m, h[j].pc - 32 + 4 * (uint64_t)w, &insn)) { OUT(" ????????"); continue; }
            OUT("%s%08x", w % 8 ? " " : "\n   ", insn);
        }
        OUT("\n");
    }
    free(h);

    /* recent synchronous exceptions */
    uint32_t ne = m->diag_nexc < 16 ? m->diag_nexc : 16;
    OUT("exceptions: %u total, last %u:\n", m->diag_nexc, ne);
    for (uint32_t k = m->diag_nexc - ne; k < m->diag_nexc; k++) {
        uint32_t i = k % 16;
        OUT("  cpu%d EL%d ec=%02llx esr=%llx elr=%llx far=%llx\n", m->diag_exc[i].cpu, m->diag_exc[i].from_el,
            (unsigned long long)(m->diag_exc[i].esr >> 26), (unsigned long long)m->diag_exc[i].esr,
            (unsigned long long)m->diag_exc[i].elr, (unsigned long long)m->diag_exc[i].far);
    }
    if (n >= 0 && (size_t)n < len) n += (int)wd_xhci_diag(m, buf + n, len - (size_t)n);
    return n < 0 ? 0 : ((size_t)n < len ? (size_t)n : len - 1);
}
