#!/usr/bin/env python3
"""Generate an AArch64 guest program that checks loads and stores in
every addressing mode against a Python memory model.

Usage: gen_mem.py OUT.S [seed] [count]
"""
import random
import sys

M64 = (1 << 64) - 1
BUF = 512
BASE = 256          # base register points at membuf + BASE


def sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


LOADS = [  # mnemonic, unscaled mnemonic, size, signed, dst 'w'/'x'
    ("ldrb", "ldurb", 1, False, "w"),
    ("ldrh", "ldurh", 2, False, "w"),
    ("ldr", "ldur", 4, False, "w"),
    ("ldr", "ldur", 8, False, "x"),
    ("ldrsb", "ldursb", 1, True, "w"),
    ("ldrsb", "ldursb", 1, True, "x"),
    ("ldrsh", "ldursh", 2, True, "w"),
    ("ldrsh", "ldursh", 2, True, "x"),
    ("ldrsw", "ldursw", 4, True, "x"),
]

STORES = [("strb", "sturb", 1, "w"), ("strh", "sturh", 2, "w"), ("str", "stur", 4, "w"), ("str", "stur", 8, "x")]


class Gen:
    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.mem = bytearray(self.rng.getrandbits(8) for _ in range(BUF))
        self.init = bytes(self.mem)
        self.lines = []
        self.n = 0

    def rd(self, off, size):
        return int.from_bytes(self.mem[off:off + size], "little")

    def wr(self, off, size, v):
        self.mem[off:off + size] = (v & ((1 << (8 * size)) - 1)).to_bytes(size, "little")

    def check(self, reg, exp, what):
        w = self.lines.append
        w(f"    mov x20, {reg}")
        w(f"    ldr x0, ={self.n}")
        w("    mov x1, x20")
        w(f"    ldr x2, ={exp & M64:#x}")
        w("    mov x3, #0")
        w("    mov x4, #0")
        w("    mov x5, #0")
        w("    bl check")
        self.names.append(what)
        self.n += 1

    def save(self):
        self.lines.append("    mov x22, x0")
        self.lines.append("    mov x23, x1")
        self.lines.append("    mov x24, x3")

    def setup(self, x2=0, x0=0, x3=0):
        w = self.lines.append
        w(f"    ldr x1, =membuf + {BASE}")
        w(f"    ldr x2, ={x2 & M64:#x}")
        w(f"    ldr x0, ={x0 & M64:#x}")
        w(f"    ldr x3, ={x3 & M64:#x}")

    def addr_mode(self, size, allow_unscaled=True):
        """Pick an addressing mode; returns (operand text, ea offset in buf, writeback offset, x2, kind)."""
        rng = self.rng
        lo, hi = 0, BUF - size
        kind = rng.choice(["uoff", "unscaled", "pre", "post", "reg", "reg_lsl", "sxtw", "uxtw", "sxtx"])
        if kind == "uoff":
            k = rng.randrange(0, (hi - BASE) // size + 1)
            imm = k * size
            return f"[x1, #{imm}]", BASE + imm, None, 0, kind
        if kind in ("unscaled", "pre", "post"):
            imm = rng.randrange(-256, 256)
            if not (lo <= BASE + imm <= hi):
                imm = rng.randrange(-BASE, hi - BASE + 1)
            if kind == "unscaled":
                return f"[x1, #{imm}]", BASE + imm, None, 0, kind
            if kind == "pre":
                return f"[x1, #{imm}]!", BASE + imm, BASE + imm, 0, kind
            return f"[x1], #{imm}", BASE, BASE + imm, 0, kind
        if kind == "reg":
            idx = rng.randrange(lo - BASE, hi - BASE + 1)
            return "[x1, x2]", BASE + idx, None, idx, kind
        if kind == "reg_lsl":
            sh = {1: 0, 2: 1, 4: 2, 8: 3}[size]
            idx = rng.randrange((lo - BASE) // size, (hi - BASE) // size + 1)
            return f"[x1, x2, lsl #{sh}]", BASE + idx * size, None, idx, kind
        if kind == "sxtw":
            idx = rng.randrange(lo - BASE, hi - BASE + 1)
            # upper 32 bits of x2 are garbage and must be ignored
            x2 = (idx & 0xFFFFFFFF) | (rng.getrandbits(32) << 32)
            return "[x1, w2, sxtw]", BASE + idx, None, x2, kind
        if kind == "uxtw":
            idx = rng.randrange(0, hi - BASE + 1)
            x2 = idx | (rng.getrandbits(32) << 32)
            return "[x1, w2, uxtw]", BASE + idx, None, x2, kind
        idx = rng.randrange(lo - BASE, hi - BASE + 1)
        return "[x1, x2, sxtx]", BASE + idx, None, idx, kind

    def one(self):
        rng = self.rng
        w = self.lines.append
        k = rng.random()
        if k < 0.45:
            mn, umn, size, signed, dst = rng.choice(LOADS)
            opnd, ea, wb, x2, kind = self.addr_mode(size)
            if kind == "unscaled":
                mn = umn
            self.setup(x2=x2, x0=rng.getrandbits(64))
            w(f"    {mn} {dst}0, {opnd}")
            self.save()
            v = self.rd(ea, size)
            if signed:
                v = sx(v, 8 * size) & (M64 if dst == "x" else 0xFFFFFFFF)
            self.check("x22", v, f"{mn} {dst}0, {opnd} @ {ea}")
            if wb is not None:
                w("    ldr x4, =membuf")
                w("    sub x23, x23, x4")
                self.check("x23", wb, f"{mn} writeback @ {wb}")
        elif k < 0.75:
            mn, umn, size, src = rng.choice(STORES)
            opnd, ea, wb, x2, kind = self.addr_mode(size)
            if kind == "unscaled":
                mn = umn
            val = rng.getrandbits(64)
            self.setup(x2=x2, x0=val)
            w(f"    {mn} {src}0, {opnd}")
            self.save()
            self.wr(ea, size, val)
            if wb is not None:
                w("    ldr x4, =membuf")
                w("    sub x23, x23, x4")
                self.check("x23", wb, f"{mn} writeback")
            a0 = ea & ~7
            for off in sorted({a0, (ea + size - 1) & ~7}):
                w(f"    ldr x6, =membuf + {off}")
                w("    ldr x6, [x6]")
                self.check("x6", self.rd(off, 8), f"{mn} {src}0, {opnd} -> word {off}")
        else:
            kind = rng.choice(["ldp_w", "ldp_x", "ldpsw", "stp_w", "stp_x"])
            size = 8 if kind in ("ldp_x", "stp_x") else 4
            mode = rng.choice(["off", "pre", "post"])
            lim = 63
            while True:
                k7 = rng.randrange(-64, 64)
                ea_off = BASE + (k7 * size if mode != "post" else 0)
                if 0 <= ea_off <= BUF - 2 * size and 0 <= BASE + k7 * size <= BUF:
                    break
            imm = k7 * size
            opnd = {"off": f"[x1, #{imm}]", "pre": f"[x1, #{imm}]!", "post": f"[x1], #{imm}"}[mode]
            wb = BASE + imm if mode != "off" else None
            r = "x" if size == 8 else "w"
            if kind.startswith("ld"):
                self.setup(x0=rng.getrandbits(64), x3=rng.getrandbits(64))
                mn = "ldpsw" if kind == "ldpsw" else "ldp"
                rr = "x" if kind == "ldpsw" else r
                w(f"    {mn} {rr}0, {rr}3, {opnd}")
                self.save()
                v1, v2 = self.rd(ea_off, size), self.rd(ea_off + size, size)
                if kind == "ldpsw":
                    v1, v2 = sx(v1, 32) & M64, sx(v2, 32) & M64
                self.check("x22", v1, f"{mn} first {opnd}")
                self.check("x24", v2, f"{mn} second {opnd}")
            else:
                v1, v2 = rng.getrandbits(64), rng.getrandbits(64)
                self.setup(x0=v1, x3=v2)
                w(f"    stp {r}0, {r}3, {opnd}")
                self.save()
                self.wr(ea_off, size, v1)
                self.wr(ea_off + size, size, v2)
                for off in sorted({ea_off & ~7, (ea_off + 2 * size - 1) & ~7}):
                    w(f"    ldr x6, =membuf + {off}")
                    w("    ldr x6, [x6]")
                    self.check("x6", self.rd(off, 8), f"stp -> word {off}")
            if wb is not None:
                w("    ldr x4, =membuf")
                w("    sub x23, x23, x4")
                self.check("x23", wb, f"pair writeback {opnd}")
        if len(self.lines) - self.last_pool > 600:
            w("    b 1f")
            w("    .ltorg")
            w("1:")
            self.last_pool = len(self.lines)

    def build(self, count):
        self.names = []
        self.last_pool = 0
        for _ in range(count):
            self.one()

    def emit(self, path):
        out = ["/* generated by tests/gen_mem.py - do not edit */", "    .data", "    .balign 16",
               "    .global membuf", "membuf:"]
        for i in range(0, BUF, 16):
            out.append("    .byte " + ", ".join(str(b) for b in self.init[i:i + 16]))
        out.append("    .space 16")
        out += ["    .text", "    .global run_cases", "run_cases:", "    stp x29, x30, [sp, #-64]!", "    stp x19, x20, [sp, #16]",
                "    stp x21, x22, [sp, #32]", "    stp x23, x24, [sp, #48]"]
        out += self.lines
        out += ["    ldp x19, x20, [sp, #16]", "    ldp x21, x22, [sp, #32]", "    ldp x23, x24, [sp, #48]",
                "    ldp x29, x30, [sp], #64", "    ret", "    .ltorg", "    .section .rodata", "    .balign 8",
                "    .global case_count", f"case_count: .quad {self.n}", "    .global case_names", "case_names:"]
        out += [f"    .quad name_{i}" for i in range(self.n)]
        out += [f"name_{i}: .asciz \"{nm}\"" for i, nm in enumerate(self.names)]
        open(path, "w").write("\n".join(out) + "\n")


def main():
    path = sys.argv[1]
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 777
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 2500
    g = Gen(seed)
    g.build(count)
    g.emit(path)


if __name__ == "__main__":
    main()
