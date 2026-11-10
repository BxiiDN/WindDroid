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


static int peek8(wd_machine *m, uint64_t va, uint8_t *out)
{
    uint64_t q;
    if (wd_peek_u64(m, va & ~7ULL, &q)) return -1;
    *out = (uint8_t)(q >> (8 * (va & 7)));
    return 0;
}

static int peek32(wd_machine *m, uint64_t va, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b;
        if (peek8(m, va + (uint64_t)i, &b)) return -1;
        v |= (uint32_t)b << (8 * i);
    }
    *out = v;
    return 0;
}

/* the PE image (firmware driver, boot loader) containing va: base and the file name from its CodeView record */
static int find_module(wd_machine *m, uint64_t va, uint64_t *base, char *name, int nlen)
{
    name[0] = 0;
    for (uint64_t p = va & ~0xFFFULL, k = 0; k < 8192 && p; k++, p -= 0x1000) {
        uint32_t mz, lfanew, sig, size, dbg_rva, dbg_len;
        if (peek32(m, p, &mz) || (mz & 0xFFFF) != 0x5A4D) continue;
        if (peek32(m, p + 0x3C, &lfanew) || lfanew > 0x1000 || peek32(m, p + lfanew, &sig) || sig != 0x4550) continue;
        uint64_t opt = p + lfanew + 24;
        uint32_t magic;
        if (peek32(m, opt, &magic) || (magic & 0xFFFF) != 0x20B || peek32(m, opt + 56, &size)) continue;
        if (va >= p + size) return -1;
        *base = p;
        if (peek32(m, opt + 112 + 6 * 8, &dbg_rva) || peek32(m, opt + 112 + 6 * 8 + 4, &dbg_len)) return 0;
        for (uint32_t e = 0; e + 28 <= dbg_len && e < 28 * 8; e += 28) {
            uint32_t type, raw;
            if (peek32(m, p + dbg_rva + e + 12, &type) || peek32(m, p + dbg_rva + e + 20, &raw) || type != 2) continue;
            uint32_t cv;
            if (peek32(m, p + raw, &cv)) break;
            uint64_t str = cv == 0x53445352 ? p + raw + 24 : cv == 0x3031424E ? p + raw + 16 : 0;   /* RSDS / NB10 */
            if (!str) break;
            int k2 = 0;
            for (int i = 0; i < 512; i++) {                 /* keep only the part after the last slash */
                uint8_t b;
                if (peek8(m, str + (uint64_t)i, &b) || !b) break;
                if (b == '/' || b == '\\') { k2 = 0; continue; }
                if (k2 < nlen - 1) name[k2++] = (char)b;
            }
            name[k2] = 0;
            break;
        }
        return 0;
    }
    return -1;
}

static void where(wd_machine *m, char *buf, size_t len, int *pn, uint64_t va)
{
    int n = *pn;
    uint64_t base;
    char name[96];
    if (find_module(m, va, &base, name, sizeof name) == 0)
        OUT(" %llx(%s+%llx)", (unsigned long long)va, name[0] ? name : "?", (unsigned long long)(va - base));
    else
        OUT(" %llx", (unsigned long long)va);
    *pn = n;
}

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
    /* frame-pointer backtrace (current CPU's translation regime; fine for firmware and boot loaders) */
    if (k == m->cur) {
        OUT("     at");
        where(m, buf, len, &n, c->pc);
        OUT("\n     backtrace:");
        where(m, buf, len, &n, c->x[30]);
        uint64_t fp = c->x[29];
        for (int i = 0; i < 12 && fp; i++) {
            uint64_t next, lr;
            if (wd_peek_u64(m, fp, &next) || wd_peek_u64(m, fp + 8, &lr) || next <= fp) break;
            where(m, buf, len, &n, lr);
            fp = next;
        }
        OUT("\n");
    }
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
    {
        OUT("  %3u%% cpu%d", ns ? h[j].count * 100 / ns : 0, h[j].cpu);
        where(m, buf, len, &n, h[j].pc);
        OUT("\n");
    }
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
