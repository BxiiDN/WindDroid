#!/usr/bin/env python3
"""Mutation testing: inject known bugs into the engine one at a time and
check that the test suite catches every one of them.

Usage: mutation_test.py [WORD...]   (run from the repository root after `make test`;
       WORDs select the mutants whose description contains one of them)
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (description, file, original text, mutated text)
MUTANTS = [
    ("overflow flag always clear", "core/cpu.c",
     "v = ((~(a ^ b) & (a ^ res)) >> 63) & 1;", "v = 0;"),
    ("signed GE/LT condition inverted", "core/cpu.c",
     "case 5: r = n == v; break;", "case 5: r = n != v; break;"),
    ("bitmask rotation off by one", "core/cpu.c",
     "if (R) welem = ((welem >> R) | (welem << (esize - R))) & emask;",
     "if (R > 1) welem = ((welem >> R) | (welem << (esize - R))) & emask;"),
    ("32-bit signed load not truncated", "core/cpu.c",
     "if (!dst64) v = (uint32_t)v;", ";"),
    ("pre-index writeback skipped", "core/cpu.c",
     "default: addr = base + off; wb = 1; break;              /* pre-index */",
     "default: addr = base + off; break;"),
    ("read-only pages writable", "core/mmu.c",
     "if (!(ap & 2)) perm |= P_W1;", "perm |= P_W1;"),
    ("EOI does not deactivate", "devices/gic.c",
     "if (!(g->ctlr & 2)) g->active[id] = 0;", ";"),
    ("CLS off by one", "core/cpu.c",
     "for (int i = size - 2; i >= 0 && !((t >> i) & 1); i--) n++;",
     "for (int i = size - 1; i >= 0 && !((t >> i) & 1); i--) n++;"),
    ("CRC32C uses CRC32 polynomial", "core/cpu.c",
     "uint32_t poly = castagnoli ? 0x82F63B78u : 0xEDB88320u;", "uint32_t poly = 0xEDB88320u;"),
    ("LDP second element address wrong", "core/cpu.c",
     "uint64_t v2 = wd_read(m, addr + b, b, 0);", "uint64_t v2 = wd_read(m, addr + 4, b, 0);"),
    ("SBC ignores carry", "core/cpu.c",
     "uint64_t r = add_with_carry(c, sf, X(c, rn), b, (c->nzcv >> 29) & 1, S);",
     "uint64_t r = add_with_carry(c, sf, X(c, rn), b, opn, S);"),
    ("timer never fires", "host/machine.c",
     "int v = (c->cntv_ctl & 1) && !(c->cntv_ctl & 2) && now >= c->cntv_cval;", "int v = 0;"),
    ("PAN ignored", "core/mmu.c",
     "if (c->el == 1 && !unpriv && c->pan", "if (0 && c->el == 1 && !unpriv && c->pan"),
    ("access flag ignored", "core/mmu.c",
     "if (!(desc & (1ULL << 10))) return FSC_ACCESS(level);", ";"),
    ("STXR succeeds without reservation", "core/cpu.c",
     "int ok = c->excl_valid == total && c->excl_addr == addr;", "int ok = 1;"),
    ("SVC returns to the SVC itself", "core/cpu.c",
     "wd_exception(m, EC_SVC64, imm16, 0, 0, pc + 4);", "wd_exception(m, EC_SVC64, imm16, 0, 0, pc);"),
    ("UXTW extend treated as SXTW", "core/cpu.c",
     "case 2: v = (uint32_t)v; break;", "case 2: v = (uint64_t)(int64_t)(int32_t)v; break;"),
    ("EL0 can read EL1-only pages", "core/mmu.c",
     "if (ap & 1) perm |= P_R0 | P_USER;", "perm |= P_R0; if (ap & 1) perm |= P_USER;"),
    ("FP: default NaN ignores FPCR.DN", "core/fpu.c",
     "    if (c->fpcr & FPCR_DN) r = default_nan(n);\n    return r;", "    return r;"),
    ("FP: underflow detected after rounding", "core/fpu.c",
     "            tiny = ((rz >> f.F) & ((1ULL << f.E) - 1)) == 0;", "            tiny = 0;"),
    ("FP: round-to-nearest ties away", "core/fpu.c",
     "case RM_RN: inc = cmp > 0 || (cmp == 0 && (sig & 1)); break;", "case RM_RN: inc = cmp >= 0; break;"),
    ("FP: FMA NaN order wrong", "core/fpu.c",
     "const fpv *ord[3] = { &A, &x, &y };", "const fpv *ord[3] = { &x, &y, &A };"),
    ("FP: max(+0,-0) wrong sign", "core/fpu.c",
     "int s = is_max ? (x.sign & y.sign) : (x.sign | y.sign);", "int s = x.sign;"),
    ("FP: to-int saturation not flagged", "core/fpu.c",
     "if (big || mag > lim) { c->fpsr |= FPSR_IOC;", "if (big || mag > lim) {"),
    ("FP: FRSQRTE odd/even scale swapped", "core/fpu.c",
     "unsigned scaled = (exp & 1) ? (128 | (unsigned)(fraction >> 45)) : (256 | (unsigned)(fraction >> 44));",
     "unsigned scaled = (exp & 1) ? (256 | (unsigned)(fraction >> 44)) : (128 | (unsigned)(fraction >> 45));"),
    ("SIMD: lane load selem from S bit", "core/simd.c",
     "int selem = ((((int)(insn >> 13) & 1) << 1) | R) + 1;", "int selem = (((opcode & 1) << 1) | R) + 1;"),
    ("SIMD: 64-bit vector ops keep upper half", "core/simd.c",
     "    if (!q) memset(VB(c, rd) + 8, 0, 8);\n}", "}"),
    ("SIMD: saturation does not set QC", "core/simd.c",
     "    if (qc) c->fpsr |= FPSR_QC;\n    wr_vec(c, rd, res, q);\n    return 1;\n}\n\nstatic int simd_3diff",
     "    wr_vec(c, rd, res, q);\n    return 1;\n}\n\nstatic int simd_3diff"),
    ("SIMD: ZIP uses wrong half", "core/simd.c",
     "se(res, 2 * i, esz, ge(A, i + part * half, esz));", "se(res, 2 * i, esz, ge(A, i, esz));"),
    ("SIMD: TBL out-of-range keeps old byte", "core/simd.c",
     "else if (!tbx) res[i] = 0;", ";"),
    ("SIMD: rounding shift right not rounding", "core/simd.c",
     "            if (opc & 4) v += (__int128)1 << (rsh - 1);", "            ;"),
    # disk images
    ("VHDX: BAT index ignores sector bitmap entries", "devices/image.c",
     "    return block + block / v->chunk_ratio;", "    return block;"),
    ("VHDX: log data sector leading bytes misplaced", "devices/image.c",
     "memcpy(sec, d + 8, 8);                    /* leading bytes */", "memcpy(sec, d + 4, 8);"),
    ("VHDX: log replay skipped", "devices/image.c",
     "        if (vhdx_replay_log(v, err, errlen)) goto fail;", "        ;"),
    ("VHD: sector bitmap ignored", "devices/image.c",
     "            if (bits[sec / 8] & (0x80 >> (sec % 8))) {", "            if (1) {"),
    ("VHD: newly present sector not cleared", "devices/image.c",
     "                    if (pwrite_all(v->fd, z, 512, sec_off)) return -1;", "                    ;"),
    # USB devices
    ("USB storage: CSW residue always zero", "devices/usb_storage.c",
     "        uint32_t residue = s->host_len - moved;", "        uint32_t residue = 0;"),
    ("USB storage: no padding of short data", "devices/usb_storage.c",
     "        memset(buf + n, 0, want - n);                     /* pad up to what the host announced */\n        s->done += want;\n        if (s->done >= s->host_len) s->phase = PH_CSW;\n        return (int)want;",
     "        s->done += want;\n        if (s->done >= s->host_len) s->phase = PH_CSW;\n        return (int)n;"),
    ("USB storage: partial OUT blocks dropped", "devices/usb_storage.c",
     "        memcpy(s->stage + s->staged, buf, take);", "        memset(s->stage + s->staged, 0, take);"),
    ("USB HID: modifiers ignored", "devices/usb.c",
     "        uint8_t bit = (uint8_t)(1 << (usage - 0xE0));", "        uint8_t bit = 0;"),
    ("USB HID: wLength not honoured", "devices/usb.c",
     "    if (r > length) r = length;", "    ;"),
    # firmware-level (NVMe, xHCI): checked by the UEFI disk and USB tests
    ("NVMe: PRP2 ignored for two-page transfers", "devices/nvme.c",
     "    if (rest <= PAGE) {", "    if (0) {", "fw"),
    ("NVMe: completion phase tag not inverted on wrap", "devices/nvme.c",
     "    if (++q->tail == q->size) { q->tail = 0; q->phase ^= 1; }", "    if (++q->tail == q->size) { q->tail = 0; }", "fw"),
    ("xHCI: event ring cycle not toggled", "devices/xhci.c",
     "        if (x->er_seg >= x->erstsz) { x->er_seg = 0; x->er_pcs ^= 1; }",
     "        if (x->er_seg >= x->erstsz) { x->er_seg = 0; }", "fw"),
    ("xHCI: link TRB toggle cycle ignored", "devices/xhci.c",
     "        if (TRB_TYPE(t) == TR_LINK) {\n            if (t.control & TRB_TC) ccs ^= 1;",
     "        if (TRB_TYPE(t) == TR_LINK) {\n            if (0) ccs ^= 1;"),
    ("xHCI: command ring link toggle ignored", "devices/xhci.c",
     "            if (t.control & TRB_TC) x->cmd_ccs ^= 1;", "            ;"),
    ("xHCI: EHB does not gate interrupts", "devices/xhci.c",
     "    if (!(x->erdp & 8)) raise_irq(x);", "    raise_irq(x);"),
    # interrupts through the ITS
    ("GIC: LPI enable bit ignored", "devices/gic.c",
     "            if (!(cfg & 1)) continue;", "            ;"),
    ("ITS: events beyond the MAPD size accepted", "devices/its.c",
     "return d >= 0 && eventid < (1u << (s->dev[d].size + 1));", "return d >= 0;"),
    ("MSI-X: vector mask ignored", "devices/pci.c",
     "    if (msix_masked(d, vector)) {", "    if (0) {"),
    ("MSI-X: pending message lost on unmask", "devices/pci.c",
     "            d->msix_pba[v / 64] &= ~(1ULL << (v % 64));\n            msix_send(m, d, v);",
     "            d->msix_pba[v / 64] &= ~(1ULL << (v % 64));"),
    ("xHCI: port reset does not enable the port", "devices/xhci.c",
     "    p->portsc |= PORTSC_PED | PORTSC_PRC;", "    p->portsc |= PORTSC_PRC;", "fw"),
]


def run(cmd, cwd):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=1800)


def check_one(mutant, work, guests):
    """Apply one mutant in a private copy of the tree; returns (desc, status line, survived)."""
    desc, path, old, new = mutant[:4]
    fw = len(mutant) > 4
    fpath = os.path.join(work, path)
    orig = open(fpath).read()
    if orig.count(old) != 1:
        return desc, f"SKIP  {desc}: pattern not found exactly once in {path}", True
    open(fpath, "w").write(orig.replace(old, new))
    try:
        b = run(["make", "-s", "build/winddroid-vm", "build/unit_tests", "build/disktool"], work)
        if b.returncode != 0:
            return desc, f"KILL  {desc} (does not compile)", False
        u = run([os.path.join(work, "build", "unit_tests")], work)
        if u.returncode != 0:
            return desc, f"KILL  {desc}", False
        t = run([sys.executable, os.path.join(work, "tests", "run_tests.py"), "--quiet", "--no-firmware",
                 "--vm", os.path.join(work, "build", "winddroid-vm"), "--guests", guests, "--timeout", "60"], work)
        if t.returncode != 0:
            return desc, f"KILL  {desc}", False
        i = run([sys.executable, os.path.join(work, "tests", "image_tests.py")], work)
        if i.returncode != 0:
            return desc, f"KILL  {desc}", False
        if fw:
            f = run([sys.executable, os.path.join(work, "tests", "run_tests.py"), "--quiet", "--only-disks",
                     "--vm", os.path.join(work, "build", "winddroid-vm"), "--timeout", "120"], work)
            if f.returncode != 0:
                return desc, f"KILL  {desc}", False
        return desc, f"ALIVE {desc}  <-- not detected", True
    finally:
        open(fpath, "w").write(orig)


def main():
    import concurrent.futures
    survived = []
    workers = max(1, min(4, os.cpu_count() or 1))
    with tempfile.TemporaryDirectory() as tmp:
        guests = os.path.join(ROOT, "guest", "build")
        works = []
        for w in range(workers):
            work = os.path.join(tmp, f"src{w}")
            shutil.copytree(ROOT, work, ignore=shutil.ignore_patterns(".git", "apk", "build", "ext"))
            works.append(work)
        free = list(works)
        import threading
        lock = threading.Lock()

        def job(mutant):
            with lock:
                work = free.pop()
            try:
                return check_one(mutant, work, guests)
            finally:
                with lock:
                    free.append(work)

        chosen = [mu for mu in MUTANTS if len(sys.argv) < 2 or any(k in mu[0] for k in sys.argv[1:])]
        with concurrent.futures.ThreadPoolExecutor(workers) as ex:
            for desc, line, alive in ex.map(job, chosen):
                print(line, flush=True)
                if alive:
                    survived.append(desc)
        print(f"{len(chosen) - len(survived)}/{len(chosen)} injected bugs detected")
        return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
