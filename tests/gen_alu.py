#!/usr/bin/env python3
"""Generate an AArch64 guest program that checks integer instructions
against a Python reference model.

Every case loads operands, sets NZCV, runs one instruction and calls
check(case, result, expected, nzcv, expected_nzcv, check_flags).

Usage: gen_alu.py OUT.S [seed] [count]
"""
import random
import sys

M64 = (1 << 64) - 1
M32 = (1 << 32) - 1


def mask(bits):
    return (1 << bits) - 1


def sx(v, bits):
    v &= mask(bits)
    return v - (1 << bits) if v >> (bits - 1) else v


def nzcv_of(n, z, c, v):
    return (n << 31) | (z << 30) | (c << 29) | (v << 28)


def add_flags(a, b, cin, bits):
    us = (a & mask(bits)) + (b & mask(bits)) + cin
    ss = sx(a, bits) + sx(b, bits) + cin
    r = us & mask(bits)
    n = r >> (bits - 1)
    z = int(r == 0)
    c = int(us != r)
    v = int(sx(r, bits) != ss)
    return r, nzcv_of(n, z, c, v)


def logic_flags(r, bits):
    return nzcv_of(r >> (bits - 1), int(r == 0), 0, 0)


def cond_holds(cond, f):
    n, z, c, v = (f >> 31) & 1, (f >> 30) & 1, (f >> 29) & 1, (f >> 28) & 1
    base = [z, c, n, v, c and not z, n == v, (n == v) and not z, True][cond >> 1]
    if (cond & 1) and cond != 15:
        base = not base
    return bool(base)


CONDS = ["eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"]


def shift(v, kind, amt, bits):
    v &= mask(bits)
    if amt == 0:
        return v
    if kind == "lsl":
        return (v << amt) & mask(bits)
    if kind == "lsr":
        return v >> amt
    if kind == "asr":
        return (sx(v, bits) >> amt) & mask(bits)
    return ((v >> amt) | (v << (bits - amt))) & mask(bits)


def extend(v, kind, sh):
    v = {
        "uxtb": v & 0xFF, "uxth": v & 0xFFFF, "uxtw": v & M32, "uxtx": v & M64,
        "sxtb": sx(v, 8) & M64, "sxth": sx(v, 16) & M64, "sxtw": sx(v, 32) & M64, "sxtx": v & M64,
    }[kind]
    return (v << sh) & M64


def rbit(v, bits):
    r = 0
    for i in range(bits):
        if v >> i & 1:
            r |= 1 << (bits - 1 - i)
    return r


def bswap(v, nbytes):
    return int.from_bytes((v & mask(8 * nbytes)).to_bytes(nbytes, "little"), "big")


def crc32(crc, data, nbytes, poly):
    for i in range(nbytes):
        crc ^= (data >> (8 * i)) & 0xFF
        for _ in range(8):
            crc = (crc >> 1) ^ (poly if crc & 1 else 0)
    return crc


def bitmask_imm(bits, rng):
    """Random encodable logical immediate (value only)."""
    while True:
        esize = rng.choice([2, 4, 8, 16, 32, 64] if bits == 64 else [2, 4, 8, 16, 32])
        s = rng.randrange(esize - 1)          # ones - 1, not all ones
        r = rng.randrange(esize)
        elem = mask(s + 1)
        if r:
            elem = ((elem >> r) | (elem << (esize - r))) & mask(esize)
        v = 0
        for i in range(0, bits, esize):
            v |= elem << i
        return v


INTERESTING = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
               0x100000000, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF,
               0xFFFFFFFFFFFFFFFE, 0x123456789ABCDEF0, 0xFEDCBA9876543210]


def rnd(rng):
    k = rng.random()
    if k < 0.3:
        return rng.choice(INTERESTING)
    if k < 0.5:
        return rng.getrandbits(rng.choice([8, 16, 32]))
    return rng.getrandbits(64)


class Gen:
    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.cases = []      # (asm_lines, a, b, c, d, nzcv_in, expected, expected_nzcv, checkflags)

    def add(self, asm, a, b, c, d, fin, exp, fexp=0, flags=False):
        self.cases.append((asm, a & M64, b & M64, c & M64, d & M64, fin, exp & M64, fexp, flags))

    def one(self):
        rng = self.rng
        a, b, c, d = rnd(rng), rnd(rng), rnd(rng), rnd(rng)
        fin = rng.getrandbits(4) << 28
        sf = rng.random() < 0.6
        bits = 64 if sf else 32
        r = "x" if sf else "w"
        A, B = a & mask(bits), b & mask(bits)
        kind = rng.choice(["addsub", "addsub_imm", "addsub_ext", "adc", "logic", "logic_imm", "movw",
                           "shiftv", "bitfield", "extr", "mul", "mull", "mulh", "div", "onesrc", "csel",
                           "ccmp", "crc", "neg"])
        if kind == "addsub":
            op = rng.choice(["add", "adds", "sub", "subs"])
            sk = rng.choice(["lsl", "lsr", "asr"])
            amt = rng.randrange(bits)
            bb = shift(b, sk, amt, bits)
            if op.startswith("add"):
                res, fl = add_flags(A, bb, 0, bits)
            else:
                res, fl = add_flags(A, (~bb) & mask(bits), 1, bits)
            self.add(f"{op} {r}0, {r}1, {r}2, {sk} #{amt}", a, b, c, d, fin, res,
                     fl if op.endswith("s") else 0, op.endswith("s"))
        elif kind == "addsub_imm":
            op = rng.choice(["add", "adds", "sub", "subs", "cmp", "cmn"])
            imm = rng.randrange(4096)
            sh = rng.choice([0, 12])
            v = imm << sh
            if op in ("add", "adds", "cmn"):
                res, fl = add_flags(A, v, 0, bits)
            else:
                res, fl = add_flags(A, (~v) & mask(bits), 1, bits)
            if op in ("cmp", "cmn"):
                self.add(f"{op} {r}1, #{imm}, lsl #{sh}", a, b, c, d, fin, d if True else 0, fl, True)
                # x0 is preloaded with d and must stay unchanged
                self.cases[-1] = (f"{op} {r}1, #{imm}, lsl #{sh}", a, b, c, d, fin, d, fl, True)
            else:
                self.add(f"{op} {r}0, {r}1, #{imm}, lsl #{sh}", a, b, c, d, fin, res,
                         fl if op.endswith("s") else 0, op.endswith("s"))
        elif kind == "addsub_ext":
            op = rng.choice(["add", "adds", "sub", "subs"])
            ek = rng.choice(["uxtb", "uxth", "uxtw", "sxtb", "sxth", "sxtw"] + (["uxtx", "sxtx"] if sf else []))
            sh = rng.randrange(5)
            src = f"x2" if (sf and ek in ("uxtx", "sxtx")) else "w2"
            bb = extend(b, ek, sh) & mask(bits)
            if op.startswith("add"):
                res, fl = add_flags(A, bb, 0, bits)
            else:
                res, fl = add_flags(A, (~bb) & mask(bits), 1, bits)
            self.add(f"{op} {r}0, {r}1, {src}, {ek} #{sh}", a, b, c, d, fin, res,
                     fl if op.endswith("s") else 0, op.endswith("s"))
        elif kind == "adc":
            op = rng.choice(["adc", "adcs", "sbc", "sbcs"])
            cin = (fin >> 29) & 1
            if op.startswith("adc"):
                res, fl = add_flags(A, B, cin, bits)
            else:
                res, fl = add_flags(A, (~B) & mask(bits), cin, bits)
            self.add(f"{op} {r}0, {r}1, {r}2", a, b, c, d, fin, res, fl if op.endswith("s") else 0,
                     op.endswith("s"))
        elif kind == "neg":
            op = rng.choice(["neg", "negs", "ngc", "ngcs", "mvn"])
            cin = (fin >> 29) & 1
            if op in ("neg", "negs"):
                res, fl = add_flags(0, (~B) & mask(bits), 1, bits)
            elif op in ("ngc", "ngcs"):
                res, fl = add_flags(0, (~B) & mask(bits), cin, bits)
            else:
                res, fl = (~B) & mask(bits), 0
            self.add(f"{op} {r}0, {r}2", a, b, c, d, fin, res, fl if op.endswith("s") else 0, op.endswith("s"))
        elif kind == "logic":
            op = rng.choice(["and", "ands", "orr", "eor", "bic", "bics", "orn", "eon"])
            sk = rng.choice(["lsl", "lsr", "asr", "ror"])
            amt = rng.randrange(bits)
            bb = shift(b, sk, amt, bits)
            if op in ("bic", "bics", "orn", "eon"):
                bb = (~bb) & mask(bits)
            res = {"and": A & bb, "ands": A & bb, "bic": A & bb, "bics": A & bb,
                   "orr": A | bb, "orn": A | bb, "eor": A ^ bb, "eon": A ^ bb}[op]
            fl = logic_flags(res, bits)
            s = op in ("ands", "bics")
            self.add(f"{op} {r}0, {r}1, {r}2, {sk} #{amt}", a, b, c, d, fin, res, fl if s else 0, s)
        elif kind == "logic_imm":
            op = rng.choice(["and", "ands", "orr", "eor", "tst"])
            imm = bitmask_imm(bits, rng)
            res = {"and": A & imm, "ands": A & imm, "tst": A & imm, "orr": A | imm, "eor": A ^ imm}[op]
            fl = logic_flags(res, bits)
            if op == "tst":
                self.add(f"tst {r}1, #{imm:#x}", a, b, c, d, fin, d, fl, True)
            else:
                self.add(f"{op} {r}0, {r}1, #{imm:#x}", a, b, c, d, fin, res, fl if op == "ands" else 0,
                         op == "ands")
        elif kind == "movw":
            op = rng.choice(["movz", "movn", "movk"])
            imm = rng.randrange(65536)
            hw = rng.randrange(4 if sf else 2)
            if op == "movz":
                res = imm << (16 * hw)
            elif op == "movn":
                res = (~(imm << (16 * hw))) & mask(bits)
            else:
                res = ((d & mask(bits)) & ~(0xFFFF << (16 * hw))) | (imm << (16 * hw))
            self.add(f"{op} {r}0, #{imm}, lsl #{16 * hw}", a, b, c, d, fin, res)
        elif kind == "shiftv":
            op = rng.choice(["lsl", "lsr", "asr", "ror"])
            amt = B & (bits - 1)
            self.add(f"{op} {r}0, {r}1, {r}2", a, b, c, d, fin, shift(A, op, amt, bits))
        elif kind == "bitfield":
            op = rng.choice(["ubfm", "sbfm", "bfm"])
            immr, imms = rng.randrange(bits), rng.randrange(bits)
            dst = d & mask(bits)
            if imms >= immr:
                w, pos, field = imms - immr + 1, 0, (A >> immr) & mask(imms - immr + 1)
            else:
                w, pos, field = imms + 1, bits - immr, A & mask(imms + 1)
            if op == "bfm":
                fm = mask(w) << pos
                res = (dst & ~fm) | ((field << pos) & fm)
            else:
                if op == "sbfm":
                    field = sx(field, w) & M64
                res = (field << pos) & mask(bits)
            self.add(f"{op} {r}0, {r}1, #{immr}, #{imms}", a, b, c, d, fin, res & mask(bits))
        elif kind == "extr":
            lsb = rng.randrange(bits)
            res = ((A << bits | B) >> lsb) & mask(bits)
            self.add(f"extr {r}0, {r}1, {r}2, #{lsb}", a, b, c, d, fin, res)
        elif kind == "mul":
            op = rng.choice(["madd", "msub", "mul", "mneg"])
            C = c & mask(bits)
            p = A * B
            res = {"madd": C + p, "msub": C - p, "mul": p, "mneg": -p}[op] & mask(bits)
            if op in ("madd", "msub"):
                self.add(f"{op} {r}0, {r}1, {r}2, {r}3", a, b, c, d, fin, res)
            else:
                self.add(f"{op} {r}0, {r}1, {r}2", a, b, c, d, fin, res)
        elif kind == "mull":
            op = rng.choice(["smaddl", "smsubl", "umaddl", "umsubl", "smull", "umull"])
            a32, b32 = a & M32, b & M32
            p = sx(a32, 32) * sx(b32, 32) if op[0] == "s" else a32 * b32
            if "sub" in op:
                res = (c - p) & M64
                self.add(f"{op} x0, w1, w2, x3", a, b, c, d, fin, res)
            elif "add" in op:
                res = (c + p) & M64
                self.add(f"{op} x0, w1, w2, x3", a, b, c, d, fin, res)
            else:
                self.add(f"{op} x0, w1, w2", a, b, c, d, fin, p & M64)
        elif kind == "mulh":
            op = rng.choice(["smulh", "umulh"])
            p = sx(a, 64) * sx(b, 64) if op == "smulh" else a * b
            self.add(f"{op} x0, x1, x2", a, b, c, d, fin, (p >> 64) & M64)
        elif kind == "div":
            op = rng.choice(["udiv", "sdiv"])
            if rng.random() < 0.1:
                b = 0
                B = 0
            if rng.random() < 0.05:
                a, b = (1 << (bits - 1)), mask(bits)
                A, B = a, b
            if B == 0:
                res = 0
            elif op == "udiv":
                res = A // B
            else:
                x, y = sx(A, bits), sx(B, bits)
                q = abs(x) // abs(y)
                if (x < 0) != (y < 0):
                    q = -q
                res = q & mask(bits)
            self.add(f"{op} {r}0, {r}1, {r}2", a, b, c, d, fin, res)
        elif kind == "onesrc":
            ops = ["rbit", "rev", "rev16", "clz", "cls"] + (["rev32"] if sf else [])
            op = rng.choice(ops)
            if op == "rbit":
                res = rbit(A, bits)
            elif op == "rev":
                res = bswap(A, bits // 8)
            elif op == "rev16":
                res = 0
                for i in range(0, bits, 16):
                    h = (A >> i) & 0xFFFF
                    res |= bswap(h, 2) << i
            elif op == "rev32":
                res = bswap(A & M32, 4) | (bswap(A >> 32, 4) << 32)
            elif op == "clz":
                res = bits - A.bit_length()
            else:
                t = ((A >> 1) ^ A) & mask(bits - 1)
                res = (bits - 1) - t.bit_length()
            self.add(f"{op} {r}0, {r}1", a, b, c, d, fin, res)
        elif kind == "csel":
            op = rng.choice(["csel", "csinc", "csinv", "csneg"])
            cond = rng.randrange(16)
            if cond_holds(cond, fin):
                res = A
            else:
                res = {"csel": B, "csinc": B + 1, "csinv": ~B, "csneg": -B}[op] & mask(bits)
            self.add(f"{op} {r}0, {r}1, {r}2, {CONDS[cond]}", a, b, c, d, fin, res)
        elif kind == "ccmp":
            op = rng.choice(["ccmp", "ccmn"])
            cond = rng.randrange(16)
            nz = rng.randrange(16)
            use_imm = rng.random() < 0.4
            bb = rng.randrange(32) if use_imm else B
            if cond_holds(cond, fin):
                if op == "ccmp":
                    _, fl = add_flags(A, (~bb) & mask(bits), 1, bits)
                else:
                    _, fl = add_flags(A, bb, 0, bits)
            else:
                fl = nz << 28
            src = f"#{bb}" if use_imm else f"{r}2"
            self.add(f"{op} {r}1, {src}, #{nz}, {CONDS[cond]}", a, b, c, d, fin, d, fl, True)
        elif kind == "crc":
            sz = rng.choice(["b", "h", "w", "x"])
            cc = rng.random() < 0.5
            nb = {"b": 1, "h": 2, "w": 4, "x": 8}[sz]
            poly = 0x82F63B78 if cc else 0xEDB88320
            res = crc32(a & M32, b, nb, poly)
            src = "x2" if sz == "x" else "w2"
            self.add(f"crc32{'c' if cc else ''}{sz} w0, w1, {src}", a, b, c, d, fin, res)

    def emit(self, path):
        out = []
        w = out.append
        w("/* generated by tests/gen_alu.py - do not edit */")
        w("    .text")
        w("    .global run_cases")
        w("run_cases:")
        w("    stp x29, x30, [sp, #-64]!"); w("    stp x19, x20, [sp, #16]"); w("    stp x21, x22, [sp, #32]"); w("    stp x23, x24, [sp, #48]")
        for i, (asm, a, b, c, d, fin, exp, fexp, flags) in enumerate(self.cases):
            w(f"    // case {i}: {asm}")
            w(f"    ldr x1, ={a:#x}")
            w(f"    ldr x2, ={b:#x}")
            w(f"    ldr x3, ={c:#x}")
            w(f"    ldr x0, ={d:#x}")
            w(f"    ldr x9, ={fin:#x}")
            w("    msr nzcv, x9")
            w(f"    {asm}")
            w("    mrs x21, nzcv")
            w("    mov x20, x0")
            w(f"    ldr x0, ={i}")
            w("    mov x1, x20")
            w(f"    ldr x2, ={exp:#x}")
            w("    mov x3, x21")
            w(f"    ldr x4, ={fexp:#x}")
            w(f"    mov x5, #{1 if flags else 0}")
            w("    bl check")
            if i % 40 == 39:
                w("    b 1f")
                w("    .ltorg")
                w("1:")
        w("    ldp x19, x20, [sp, #16]"); w("    ldp x21, x22, [sp, #32]"); w("    ldp x23, x24, [sp, #48]"); w("    ldp x29, x30, [sp], #64")
        w("    ret")
        w("    .ltorg")
        w(f"    .global case_count")
        w("    .section .rodata")
        w("    .balign 8")
        w(f"case_count: .quad {len(self.cases)}")
        w("    .section .rodata.names, \"a\"")
        w("    .global case_names")
        w("case_names:")
        for i, case in enumerate(self.cases):
            w(f"    .quad name_{i}")
        for i, case in enumerate(self.cases):
            w(f"name_{i}: .asciz \"{case[0]}\"")
        open(path, "w").write("\n".join(out) + "\n")


def main():
    path = sys.argv[1]
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 12345
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 4000
    g = Gen(seed)
    for _ in range(count):
        g.one()
    g.emit(path)


if __name__ == "__main__":
    main()
