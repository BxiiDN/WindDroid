/*
 * WindDroid - stage 1 address translation (EL1&0 regime, 4 KiB granule)
 * and guest memory access.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "internal.h"

#define FSC_ADDR_SIZE(l)   (0x00 + (l))
#define FSC_TRANS(l)       (0x04 + (l))
#define FSC_ACCESS(l)      (0x08 + (l))
#define FSC_PERM(l)        (0x0C + (l))
#define FSC_EXT_ABORT      0x10
#define FSC_WALK_EXT(l)    (0x14 + (l))
#define FSC_ALIGN          0x21

#define OA_MASK 0x0000FFFFFFFFF000ULL   /* 48-bit output addresses */

void wd_tlb_flush(wd_machine *m)
{
    for (unsigned i = 0; i < TLB_SIZE; i++) m->tlb[i].vpn = ~0ULL;
    for (int k = 0; k < 3; k++)
        for (unsigned i = 0; i < FTLB_SIZE; i++) m->ftlb_r[k][i].vpn = m->ftlb_w[k][i].vpn = ~0ULL;
    m->fetch_vpn = ~0ULL;
}

typedef struct {
    uint64_t pa_page;
    uint8_t perm;
    uint8_t level;
    uint8_t attr;
    uint8_t sh;
} walk_result;

/* Returns 0 on success or a fault status code (FSC). */
static int walk(wd_machine *m, uint64_t va, walk_result *r)
{
    wd_cpu *c = &m->cpu;

    if (!(c->sctlr & 1)) {                          /* MMU disabled: flat mapping */
        if (va >> 48) return FSC_ADDR_SIZE(0);
        r->pa_page = va & OA_MASK;
        r->perm = P_R0 | P_W0 | P_X0 | P_R1 | P_W1 | P_X1;
        r->level = 0;
        r->attr = 0;
        r->sh = 0;
        return 0;
    }

    uint64_t tcr = c->tcr;
    int top = (va >> 55) & 1;
    int tsz = top ? (int)((tcr >> 16) & 63) : (int)(tcr & 63);
    int epd = top ? (int)((tcr >> 23) & 1) : (int)((tcr >> 7) & 1);
    int tg = top ? (int)((tcr >> 30) & 3) : (int)((tcr >> 14) & 3);
    int tbi = top ? (int)((tcr >> 38) & 1) : (int)((tcr >> 37) & 1);
    int gran_ok = top ? tg == 2 : tg == 0;

    if (tsz < 16) tsz = 16;
    if (tsz > 39) tsz = 39;
    int ia = 64 - tsz;

    uint64_t chk = tbi ? wd_sext(va, 56) : va;
    uint64_t hi = chk >> ia;
    if (top ? hi != wd_mask(64 - ia) : hi != 0) return FSC_TRANS(0);
    if (epd || !gran_ok) return FSC_TRANS(0);

    int level = ia > 39 ? 0 : (ia > 30 ? 1 : 2);
    uint64_t table = (top ? c->ttbr1 : c->ttbr0) & 0x0000FFFFFFFFFFFEULL;
    uint64_t desc;

    for (;;) {
        int shift = 12 + 9 * (3 - level);
        int bits = ia - shift;
        if (bits > 9) bits = 9;
        uint64_t idx = (va >> shift) & wd_mask(bits);
        uint8_t *p = wd_phys_host(m, table + idx * 8);
        if (!p) return FSC_WALK_EXT(level);
        memcpy(&desc, p, 8);
        if (!(desc & 1)) return FSC_TRANS(level);
        if (level == 3) {
            if ((desc & 3) != 3) return FSC_TRANS(3);
            break;
        }
        if ((desc & 3) == 1) {
            if (level == 0) return FSC_TRANS(0);
            break;
        }
        table = desc & OA_MASK;
        level++;
    }

    uint64_t pa;
    if (level == 3) pa = desc & OA_MASK;
    else {
        int shift = 12 + 9 * (3 - level);
        uint64_t bmask = wd_mask(shift);
        pa = (desc & OA_MASK & ~bmask) | (va & bmask & ~0xFFFULL);
    }
    if (pa >> 40) return FSC_ADDR_SIZE(level);
    if (!(desc & (1ULL << 10))) return FSC_ACCESS(level);

    int ap = (desc >> 6) & 3;
    int uxn = (desc >> 54) & 1, pxn = (desc >> 53) & 1;
    int wxn = (c->sctlr >> 19) & 1;
    uint8_t perm = P_R1;
    if (!(ap & 2)) perm |= P_W1;
    if (ap & 1) perm |= P_R0 | P_USER;
    if (ap == 1) perm |= P_W0;
    if (!uxn && !(wxn && (perm & P_W0))) perm |= P_X0;
    if (!pxn && ap != 1 && !(wxn && (perm & P_W1))) perm |= P_X1;

    r->pa_page = pa;
    r->perm = perm;
    r->level = (uint8_t)level;
    r->attr = (desc >> 2) & 7;
    r->sh = (desc >> 8) & 3;
    return 0;
}

static _Noreturn void raise_abort(wd_machine *m, uint64_t va, int acc, int fsc)
{
    wd_cpu *c = &m->cpu;
    if (acc == ACC_EXEC) {
        wd_exception(m, c->el == 0 ? EC_IABT_LOW : EC_IABT_CUR, (uint32_t)fsc, va, 1, c->pc);
    }
    uint32_t iss = (uint32_t)fsc | (acc == ACC_WRITE ? (1u << 6) : 0);
    wd_exception(m, c->el == 0 ? EC_DABT_LOW : EC_DABT_CUR, iss, va, 1, c->pc);
}

static int perm_ok(wd_machine *m, const wd_tlbe *e, int acc, int unpriv)
{
    wd_cpu *c = &m->cpu;
    int el0 = c->el == 0 || unpriv;
    uint8_t need;
    if (acc == ACC_EXEC) need = c->el == 0 ? P_X0 : P_X1;
    else if (acc == ACC_WRITE) need = el0 ? P_W0 : P_W1;
    else need = el0 ? P_R0 : P_R1;
    if (!(e->perm & need)) return 0;
    if (c->el == 1 && !unpriv && c->pan && acc != ACC_EXEC && (e->perm & P_USER) && (c->sctlr & 1))
        return 0;
    return 1;
}

wd_tlbe *wd_translate(wd_machine *m, uint64_t va, int acc, int unpriv)
{
    uint64_t vpn = va >> 12;
    wd_tlbe *e = &m->tlb[vpn & (TLB_SIZE - 1)];
    if (e->vpn != vpn) {
        walk_result r;
        int fsc = walk(m, va, &r);
        if (fsc) raise_abort(m, va, acc, fsc);
        e->vpn = vpn;
        e->pa_page = r.pa_page;
        e->perm = r.perm;
        e->level = r.level;
        e->attr = r.attr;
        e->host = wd_phys_host(m, r.pa_page);
        e->ro = !wd_phys_is_ram(m, r.pa_page);
    }
    if (!perm_ok(m, e, acc, unpriv)) raise_abort(m, va, acc, FSC_PERM(e->level ? e->level : 3));
    return e;
}

void wd_at(wd_machine *m, uint64_t va, int acc, int unpriv)
{
    wd_cpu *c = &m->cpu;
    walk_result r;
    int fsc = walk(m, va, &r);
    if (!fsc) {
        wd_tlbe tmp = { .perm = r.perm, .level = r.level };
        if (!perm_ok(m, &tmp, acc, unpriv)) fsc = FSC_PERM(r.level ? r.level : 3);
    }
    if (fsc) {
        c->par = 1 | ((uint64_t)fsc << 1);
        return;
    }
    uint64_t attr = (c->sctlr & 1) ? (c->mair >> (8 * r.attr)) & 0xFF : 0x44;
    c->par = (attr << 56) | (r.pa_page & OA_MASK) | ((uint64_t)r.sh << 7) | (1ULL << 11);
}

void wd_check_align(wd_machine *m, uint64_t va, int size, int write)
{
    if (va & (uint64_t)(size - 1)) {
        wd_cpu *c = &m->cpu;
        uint32_t iss = FSC_ALIGN | (write ? (1u << 6) : 0);
        wd_exception(m, c->el == 0 ? EC_DABT_LOW : EC_DABT_CUR, iss, va, 1, c->pc);
    }
}

uint64_t wd_read_slow(wd_machine *m, uint64_t va, int size, int unpriv)
{
    if ((m->cpu.sctlr & 2) && (va & (uint64_t)(size - 1))) wd_check_align(m, va, size, 0);
    if ((va & 0xFFF) + (uint64_t)size > 0x1000) {
        /* page crossing: translate both pages before reading anything */
        wd_translate(m, va, ACC_READ, unpriv);
        wd_translate(m, va + (uint64_t)size - 1, ACC_READ, unpriv);
        uint64_t v = 0;
        for (int i = 0; i < size; i++) v |= wd_read(m, va + (uint64_t)i, 1, unpriv) << (8 * i);
        return v;
    }
    wd_tlbe *e = wd_translate(m, va, ACC_READ, unpriv);
    if (e->host) {
        uint8_t *p = e->host + (va & 0xFFF);
        if (!unpriv) {                         /* translation and permission passed: cache it */
            unsigned i = (unsigned)(va >> 12) & (FTLB_SIZE - 1);
            int mode = wd_mem_mode(m);
            m->ftlb_r[mode][i].vpn = va >> 12;
            m->ftlb_r[mode][i].host = e->host;
        }
        switch (size) {
        case 1: return *p;
        case 2: { uint16_t v; memcpy(&v, p, 2); return v; }
        case 4: { uint32_t v; memcpy(&v, p, 4); return v; }
        default: { uint64_t v; memcpy(&v, p, 8); return v; }
        }
    }
    return wd_phys_read(m, e->pa_page | (va & 0xFFF), size);
}

void wd_write_slow(wd_machine *m, uint64_t va, int size, uint64_t val, int unpriv)
{
    if ((m->cpu.sctlr & 2) && (va & (uint64_t)(size - 1))) wd_check_align(m, va, size, 1);
    if ((va & 0xFFF) + (uint64_t)size > 0x1000) {
        wd_translate(m, va, ACC_WRITE, unpriv);
        wd_translate(m, va + (uint64_t)size - 1, ACC_WRITE, unpriv);
        for (int i = 0; i < size; i++) wd_write(m, va + (uint64_t)i, 1, val >> (8 * i), unpriv);
        return;
    }
    wd_tlbe *e = wd_translate(m, va, ACC_WRITE, unpriv);
    if (e->host && !e->ro) {
        uint8_t *p = e->host + (va & 0xFFF);
        int code = 0;
        if (m->code_page && e->host >= m->ram && e->host < m->ram + m->ram_size) {
            uint64_t page = (uint64_t)(e->host - m->ram) >> 12;
            if (m->code_page[page]) { code = 1; wd_jit_page_written(m, page); }
        }
        if (!unpriv && !code) {
            unsigned i = (unsigned)(va >> 12) & (FTLB_SIZE - 1);
            int mode = wd_mem_mode(m);
            m->ftlb_w[mode][i].vpn = va >> 12;
            m->ftlb_w[mode][i].host = e->host;
        }
        switch (size) {
        case 1: *p = (uint8_t)val; break;
        case 2: { uint16_t v = (uint16_t)val; memcpy(p, &v, 2); break; }
        case 4: { uint32_t v = (uint32_t)val; memcpy(p, &v, 4); break; }
        default: memcpy(p, &val, 8); break;
        }
        return;
    }
    wd_phys_write(m, e->pa_page | (va & 0xFFF), size, val);
}

uint32_t wd_fetch(wd_machine *m, uint64_t va)
{
    wd_cpu *c = &m->cpu;
    uint64_t vpn = va >> 12;
    if (vpn != m->fetch_vpn || m->fetch_el != c->el) {
        wd_tlbe *e = wd_translate(m, va, ACC_EXEC, 0);
        if (!e->host) raise_abort(m, va, ACC_EXEC, FSC_EXT_ABORT);
        m->fetch_vpn = vpn;
        m->fetch_host = e->host;
        m->fetch_el = c->el;
    }
    uint32_t insn;
    memcpy(&insn, m->fetch_host + (va & 0xFFF), 4);
    return insn;
}
