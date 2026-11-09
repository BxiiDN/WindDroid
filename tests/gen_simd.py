#!/usr/bin/env python3
"""Generate SIMD/FP instruction test vectors for the guest program t_simd.

Every case is one instruction (assembled by llvm-mc, an independent encoder)
with inputs v0-v3, x0, x1, FPCR, NZCV and the expected v0, x0, FPSR and NZCV
computed by tests/simd_ref.py.

Usage: gen_simd.py OUT.S [seed] [count]
"""
import os
import random
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import simd_ref as R  # noqa: E402

M64 = (1 << 64) - 1
M128 = (1 << 128) - 1
CK_V, CK_X, CK_FPSR, CK_NZCV = 1, 2, 4, 8
CONDS = ["eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"]


def cond_holds(cond, f):
    n, z, c, v = (f >> 31) & 1, (f >> 30) & 1, (f >> 29) & 1, (f >> 28) & 1
    r = [z, c, n, v, c and not z, n == v, (n == v) and not z, True][cond >> 1]
    if cond & 1 and cond != 15:
        r = not r
    return bool(r)


def lanes(v, esz, cnt):
    return [(v >> (i * esz)) & R.mask(esz) for i in range(cnt)]


def join(ls, esz):
    r = 0
    for i, x in enumerate(ls):
        r |= (x & R.mask(esz)) << (i * esz)
    return r


def arr(esz, q):
    return {8: "b", 16: "h", 32: "s", 64: "d"}[esz] if q is None else \
        f"{(128 if q else 64) // esz}{ {8: 'b', 16: 'h', 32: 's', 64: 'd'}[esz]}"


def sreg(esz):
    return {8: "b", 16: "h", 32: "s", 64: "d"}[esz]


class Gen:
    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.cases = []

    # ------------------------------------------------------------ value generators
    def fpval(self, n):
        rng = self.rng
        E, F, bias = R.FMT[n]
        k = rng.random()
        s = rng.getrandbits(1)
        if k < 0.30:
            special = [
                0, 1, R.mask(F), 1 << F, (bias << F), (bias << F) | (1 << (F - 1)),
                ((bias + 1) << F), R.max_normal(0, n), R.inf(0, n),
                R.default_nan(n), R.default_nan(n) | rng.getrandbits(F - 1),
                (R.mask(E) << F) | (rng.getrandbits(F - 1) or 1),          # signalling NaN
                ((bias - 1) << F), ((bias + F) << F) | rng.getrandbits(F), # large integers
                ((bias + 30) << F), ((bias + 62) << F), ((bias + 63) << F) | rng.getrandbits(F),
            ]
            return (s << (n - 1)) | rng.choice(special)
        if k < 0.45:                        # values near small integers / halves
            v = rng.randrange(-40, 41) / 2 + rng.choice([0, 0, 2.0 ** -F, -(2.0 ** -F)])
            st = R.St(0)
            from fractions import Fraction
            return R.fp_round(Fraction(v).limit_denominator(1 << 60), n, R.RN, st) if v else R.zero(s, n)
        if k < 0.55:                        # denormals
            return (s << (n - 1)) | rng.getrandbits(F)
        if k < 0.70:                        # moderate exponents
            e = bias + rng.randrange(-20, 21)
            return (s << (n - 1)) | (e << F) | rng.getrandbits(F)
        e = rng.randrange(1, R.mask(E))
        return (s << (n - 1)) | (e << F) | rng.getrandbits(F)

    def fpcr(self):
        rng = self.rng
        v = rng.randrange(4) << 22
        if rng.random() < 0.2:
            v |= 1 << 25
        if rng.random() < 0.2:
            v |= 1 << 24
        return v

    def ival(self, bits):
        rng = self.rng
        k = rng.random()
        if k < 0.25:
            return rng.choice([0, 1, R.mask(bits), R.mask(bits) >> 1, 1 << (bits - 1), (1 << (bits - 1)) + 1, 2, 3])
        if k < 0.4:
            return rng.getrandbits(rng.randrange(1, bits + 1))
        return rng.getrandbits(bits)

    def vec_fp(self, n):
        return join([self.fpval(n) for _ in range(128 // n)], n)

    def vec_int(self, esz):
        return join([self.ival(esz) for _ in range(128 // esz)], esz)

    def add(self, asm, ins, exp, check):
        """ins: dict q0..q3,x0,x1,fpcr,nzcv ; exp: dict q0..q3,x0,fpsr,nzcv.
        v1-v3 are always checked: unless stated they must keep their input value."""
        for r in ("q1", "q2", "q3"):
            exp.setdefault(r, ins[r])
        self.cases.append((asm, ins, exp, check))

    def base_inputs(self):
        rng = self.rng
        return {"q0": rng.getrandbits(128), "q1": 0, "q2": 0, "q3": 0, "x0": rng.getrandbits(64),
                "x1": rng.getrandbits(64), "fpcr": 0, "nzcv": rng.getrandbits(4) << 28}

    # ------------------------------------------------------------ scalar FP
    def scalar_fp(self):
        rng = self.rng
        n = rng.choice([32, 64])
        r = sreg(n)
        ins = self.base_inputs()
        ins["fpcr"] = self.fpcr()
        a, b, c = self.fpval(n), self.fpval(n), self.fpval(n)
        ins["q1"], ins["q2"], ins["q3"] = a, b, c
        st = R.St(ins["fpcr"])
        kind = rng.choice(["2src"] * 6 + ["fma"] * 3 + ["1src"] * 3 + ["cmp", "ccmp", "csel", "cvt", "toint",
                           "fromint", "fixed", "imm", "est", "fmovg"])
        exp = {"x0": ins["x0"], "nzcv": ins["nzcv"]}
        check = CK_V | CK_FPSR
        if kind == "2src":
            op = rng.choice(["fadd", "fsub", "fmul", "fdiv", "fmax", "fmin", "fmaxnm", "fminnm", "fnmul",
                             "fmulx", "fabd", "frecps", "frsqrts"])
            f = {"fadd": lambda: R.add(a, b, n, st), "fsub": lambda: R.add(a, b, n, st, True),
                 "fmul": lambda: R.mul(a, b, n, st), "fdiv": lambda: R.div(a, b, n, st),
                 "fmax": lambda: R.maxmin(a, b, n, st, True, False), "fmin": lambda: R.maxmin(a, b, n, st, False, False),
                 "fmaxnm": lambda: R.maxmin(a, b, n, st, True, True), "fminnm": lambda: R.maxmin(a, b, n, st, False, True),
                 "fnmul": lambda: R.neg(R.mul(a, b, n, st), n), "fmulx": lambda: R.mul(a, b, n, st, mulx=True),
                 "fabd": lambda: R.fabs(R.add(a, b, n, st, True), n), "frecps": lambda: R.recps(a, b, n, st),
                 "frsqrts": lambda: R.rsqrts(a, b, n, st)}[op]
            exp["q0"] = f()
            asm = f"{op} {r}0, {r}1, {r}2"
        elif kind == "fma":
            op = rng.choice(["fmadd", "fmsub", "fnmadd", "fnmsub"])
            if op == "fmadd":
                v = R.muladd(c, a, b, n, st)
            elif op == "fmsub":
                v = R.muladd(c, R.neg(a, n), b, n, st)
            elif op == "fnmadd":
                v = R.muladd(R.neg(c, n), R.neg(a, n), b, n, st)
            else:
                v = R.muladd(R.neg(c, n), a, b, n, st)
            exp["q0"] = v
            asm = f"{op} {r}0, {r}1, {r}2, {r}3"
        elif kind == "1src":
            op = rng.choice(["fmov", "fabs", "fneg", "fsqrt", "frintn", "frintp", "frintm", "frintz", "frinta",
                             "frintx", "frinti", "frecpx"])
            rm = {"frintn": R.RN, "frintp": R.RP, "frintm": R.RM, "frintz": R.RZ, "frinta": R.RNA,
                  "frintx": st.rmode, "frinti": st.rmode}
            if op == "fmov":
                v = a
            elif op == "fabs":
                v = R.fabs(a, n)
            elif op == "fneg":
                v = R.neg(a, n)
            elif op == "fsqrt":
                v = R.fsqrt(a, n, st)
            elif op == "frecpx":
                v = R.recpx(a, n, st)
            else:
                v = R.round_int(a, n, rm[op], op == "frintx", st)
            exp["q0"] = v
            asm = f"{op} {r}0, {r}1"
        elif kind == "cmp":
            op = rng.choice(["fcmp", "fcmpe"])
            withzero = rng.random() < 0.3
            exp["nzcv"] = R.fcmp_flags(a, 0 if withzero else b, n, st, op == "fcmpe")
            exp["q0"] = ins["q0"]
            check = CK_NZCV | CK_FPSR | CK_V
            asm = f"{op} {r}1, {'#0.0' if withzero else r + '2'}"
        elif kind == "ccmp":
            op = rng.choice(["fccmp", "fccmpe"])
            cond, nz = rng.randrange(16), rng.randrange(16)
            if cond_holds(cond, ins["nzcv"]):
                exp["nzcv"] = R.fcmp_flags(a, b, n, st, op == "fccmpe")
            else:
                exp["nzcv"] = nz << 28
            exp["q0"] = ins["q0"]
            check = CK_NZCV | CK_FPSR | CK_V
            asm = f"{op} {r}1, {r}2, #{nz}, {CONDS[cond]}"
        elif kind == "csel":
            cond = rng.randrange(16)
            exp["q0"] = a if cond_holds(cond, ins["nzcv"]) else b
            asm = f"fcsel {r}0, {r}1, {r}2, {CONDS[cond]}"
        elif kind == "cvt":
            frm, to = rng.choice([(32, 64), (64, 32), (32, 16), (16, 32), (64, 16), (16, 64)])
            x = self.fpval(frm)
            ins["q1"] = x
            exp["q0"] = R.convert(x, frm, to, st.rmode, st)
            asm = f"fcvt {sreg(to)}0, {sreg(frm)}1"
        elif kind == "toint":
            op = rng.choice(["fcvtns", "fcvtnu", "fcvtps", "fcvtpu", "fcvtms", "fcvtmu", "fcvtzs", "fcvtzu",
                             "fcvtas", "fcvtau"])
            ob = rng.choice([32, 64])
            rmode = {"n": R.RN, "p": R.RP, "m": R.RM, "z": R.RZ, "a": R.RNA}[op[4]]
            v = R.to_fixed(a, n, 0, op[5] == "u", rmode, ob, st)
            exp["x0"] = v
            exp["q0"] = ins["q0"]
            check = CK_X | CK_FPSR | CK_V
            asm = f"{op} {'x' if ob == 64 else 'w'}0, {r}1"
        elif kind == "fromint":
            op = rng.choice(["scvtf", "ucvtf"])
            ib = rng.choice([32, 64])
            x1 = self.ival(64)
            ins["x1"] = x1
            exp["q0"] = R.from_fixed(x1, op == "scvtf", ib, 0, n, st.rmode, st)
            asm = f"{op} {r}0, {'x' if ib == 64 else 'w'}1"
        elif kind == "fixed":
            ib = rng.choice([32, 64])
            fb = rng.randrange(1, ib + 1)
            if rng.random() < 0.5:
                op = rng.choice(["fcvtzs", "fcvtzu"])
                exp["x0"] = R.to_fixed(a, n, fb, op == "fcvtzu", R.RZ, ib, st)
                exp["q0"] = ins["q0"]
                check = CK_X | CK_FPSR | CK_V
                asm = f"{op} {'x' if ib == 64 else 'w'}0, {r}1, #{fb}"
            else:
                op = rng.choice(["scvtf", "ucvtf"])
                x1 = self.ival(64)
                ins["x1"] = x1
                exp["q0"] = R.from_fixed(x1, op == "scvtf", ib, fb, n, st.rmode, st)
                asm = f"{op} {r}0, {'x' if ib == 64 else 'w'}1, #{fb}"
        elif kind == "imm":
            imm8 = rng.randrange(256)
            bits = R.fp_imm(imm8, n)
            import struct
            if n == 32:
                val = struct.unpack("<f", struct.pack("<I", bits))[0]
            else:
                val = struct.unpack("<d", struct.pack("<Q", bits))[0]
            exp["q0"] = bits
            asm = f"fmov {r}0, #{val!r}"
        elif kind == "est":
            op = rng.choice(["frecpe", "frsqrte"])
            exp["q0"] = R.recpe(a, n, st) if op == "frecpe" else R.rsqrte(a, n, st)
            asm = f"{op} {r}0, {r}1"
        else:  # fmov general
            form = rng.choice(["to_w", "to_x", "from_w", "from_x", "top_to", "top_from"])
            ins["q1"] = rng.getrandbits(128)
            check = CK_V | CK_X
            exp["q0"] = ins["q0"]
            if form == "to_w":
                exp["x0"] = ins["q1"] & 0xFFFFFFFF
                asm = "fmov w0, s1"
            elif form == "to_x":
                exp["x0"] = ins["q1"] & M64
                asm = "fmov x0, d1"
            elif form == "from_w":
                exp["q0"] = ins["x1"] & 0xFFFFFFFF
                asm = "fmov s0, w1"
            elif form == "from_x":
                exp["q0"] = ins["x1"]
                asm = "fmov d0, x1"
            elif form == "top_to":
                exp["x0"] = ins["q1"] >> 64
                asm = "fmov x0, v1.d[1]"
            else:
                exp["q0"] = (ins["q0"] & M64) | (ins["x1"] << 64)
                asm = "fmov v0.d[1], x1"
            exp["fpsr"] = 0
        exp.setdefault("fpsr", st.fpsr)
        if "fpsr" not in exp or kind != "fmovg":
            exp["fpsr"] = st.fpsr
        self.add(asm, ins, exp, check)

    # ------------------------------------------------------------ vector FP
    def vector_fp(self):
        rng = self.rng
        n = rng.choice([32, 64])
        q = 1 if n == 64 else rng.choice([0, 1])
        cnt = (128 if q else 64) // n
        A = arr(n, q)
        ins = self.base_inputs()
        ins["fpcr"] = self.fpcr()
        ins["q1"], ins["q2"], ins["q3"] = self.vec_fp(n), self.vec_fp(n), self.vec_fp(n)
        ins["q0"] = self.vec_fp(n)
        st = R.St(ins["fpcr"])
        a, b, d = lanes(ins["q1"], n, cnt), lanes(ins["q2"], n, cnt), lanes(ins["q0"], n, cnt)
        kind = rng.choice(["3same"] * 4 + ["pair", "misc", "misc", "cmpz", "indexed", "cvt", "fixed", "across"])
        exp = {"x0": ins["x0"], "nzcv": ins["nzcv"]}
        allones = R.mask(n)
        if kind == "3same":
            op = rng.choice(["fadd", "fsub", "fmul", "fdiv", "fmax", "fmin", "fmaxnm", "fminnm", "fmla", "fmls",
                             "fmulx", "fabd", "frecps", "frsqrts", "fcmeq", "fcmge", "fcmgt", "facge", "facgt"])
            out = []
            for x, y, z in zip(a, b, d):
                out.append({
                    "fadd": lambda: R.add(x, y, n, st), "fsub": lambda: R.add(x, y, n, st, True),
                    "fmul": lambda: R.mul(x, y, n, st), "fdiv": lambda: R.div(x, y, n, st),
                    "fmax": lambda: R.maxmin(x, y, n, st, True, False), "fmin": lambda: R.maxmin(x, y, n, st, False, False),
                    "fmaxnm": lambda: R.maxmin(x, y, n, st, True, True),
                    "fminnm": lambda: R.maxmin(x, y, n, st, False, True),
                    "fmla": lambda: R.muladd(z, x, y, n, st), "fmls": lambda: R.muladd(z, R.neg(x, n), y, n, st),
                    "fmulx": lambda: R.mul(x, y, n, st, mulx=True), "fabd": lambda: R.fabs(R.add(x, y, n, st, True), n),
                    "frecps": lambda: R.recps(x, y, n, st), "frsqrts": lambda: R.rsqrts(x, y, n, st),
                    "fcmeq": lambda: allones if R.cmp_eq(x, y, n, st) else 0,
                    "fcmge": lambda: allones if R.cmp_ge(x, y, n, st) else 0,
                    "fcmgt": lambda: allones if R.cmp_gt(x, y, n, st) else 0,
                    "facge": lambda: allones if R.cmp_ge(R.fabs(x, n), R.fabs(y, n), n, st) else 0,
                    "facgt": lambda: allones if R.cmp_gt(R.fabs(x, n), R.fabs(y, n), n, st) else 0,
                }[op]())
            exp["q0"] = join(out, n)
            asm = f"{op} v0.{A}, v1.{A}, v2.{A}"
        elif kind == "pair":
            op = rng.choice(["faddp", "fmaxp", "fminp", "fmaxnmp", "fminnmp"])
            cat = a + b
            f = {"faddp": lambda x, y: R.add(x, y, n, st), "fmaxp": lambda x, y: R.maxmin(x, y, n, st, True, False),
                 "fminp": lambda x, y: R.maxmin(x, y, n, st, False, False),
                 "fmaxnmp": lambda x, y: R.maxmin(x, y, n, st, True, True),
                 "fminnmp": lambda x, y: R.maxmin(x, y, n, st, False, True)}[op]
            if rng.random() < 0.3:          # scalar pairwise
                exp["q0"] = f(a[0], a[1]) if cnt >= 2 else 0
                if cnt < 2:
                    return
                asm = f"{op} {sreg(n)}0, v1.2{sreg(n)}"
                if n == 32:
                    pass
            else:
                exp["q0"] = join([f(cat[2 * i], cat[2 * i + 1]) for i in range(cnt)], n)
                asm = f"{op} v0.{A}, v1.{A}, v2.{A}"
        elif kind == "misc":
            op = rng.choice(["fabs", "fneg", "fsqrt", "frintn", "frintp", "frintm", "frintz", "frinta", "frintx",
                             "frinti", "frecpe", "frsqrte", "fcvtns", "fcvtnu", "fcvtps", "fcvtpu", "fcvtms",
                             "fcvtmu", "fcvtzs", "fcvtzu", "fcvtas", "fcvtau", "scvtf", "ucvtf", "urecpe", "ursqrte"])
            if op in ("urecpe", "ursqrte") and n != 32:
                return
            rmt = {"n": R.RN, "p": R.RP, "m": R.RM, "z": R.RZ, "a": R.RNA}
            out = []
            src = a
            if op in ("scvtf", "ucvtf", "urecpe", "ursqrte"):
                ins["q1"] = self.vec_int(n)
                src = lanes(ins["q1"], n, cnt)
            for x in src:
                if op == "fabs":
                    v = R.fabs(x, n)
                elif op == "fneg":
                    v = R.neg(x, n)
                elif op == "fsqrt":
                    v = R.fsqrt(x, n, st)
                elif op.startswith("frint"):
                    m = {"frintn": R.RN, "frintp": R.RP, "frintm": R.RM, "frintz": R.RZ, "frinta": R.RNA,
                         "frintx": st.rmode, "frinti": st.rmode}[op]
                    v = R.round_int(x, n, m, op == "frintx", st)
                elif op == "frecpe":
                    v = R.recpe(x, n, st)
                elif op == "frsqrte":
                    v = R.rsqrte(x, n, st)
                elif op.startswith("fcvt"):
                    v = R.to_fixed(x, n, 0, op[5] == "u", rmt[op[4]], n, st)
                elif op in ("scvtf", "ucvtf"):
                    v = R.from_fixed(x, op == "scvtf", n, 0, n, st.rmode, st)
                elif op == "urecpe":
                    v = R.urecpe(x)
                else:
                    v = R.ursqrte(x)
                out.append(v)
            exp["q0"] = join(out, n)
            asm = f"{op} v0.{A}, v1.{A}"
        elif kind == "cmpz":
            op = rng.choice(["fcmeq", "fcmge", "fcmgt", "fcmle", "fcmlt"])
            out = []
            for x in a:
                if op == "fcmeq":
                    t = R.cmp_eq(x, 0, n, st)
                elif op == "fcmge":
                    t = R.cmp_ge(x, 0, n, st)
                elif op == "fcmgt":
                    t = R.cmp_gt(x, 0, n, st)
                elif op == "fcmle":
                    t = R.cmp_ge(0, x, n, st)
                else:
                    t = R.cmp_gt(0, x, n, st)
                out.append(allones if t else 0)
            exp["q0"] = join(out, n)
            asm = f"{op} v0.{A}, v1.{A}, #0.0"
        elif kind == "indexed":
            op = rng.choice(["fmla", "fmls", "fmul", "fmulx"])
            idx = rng.randrange(128 // n)
            e2 = lanes(ins["q2"], n, 128 // n)[idx]
            scalar = rng.random() < 0.3
            src = a[:1] if scalar else a
            dd = d[:1] if scalar else d
            out = []
            for x, z in zip(src, dd):
                out.append({"fmla": lambda: R.muladd(z, x, e2, n, st),
                            "fmls": lambda: R.muladd(z, R.neg(x, n), e2, n, st),
                            "fmul": lambda: R.mul(x, e2, n, st),
                            "fmulx": lambda: R.mul(x, e2, n, st, mulx=True)}[op]())
            exp["q0"] = join(out, n)
            if scalar:
                asm = f"{op} {sreg(n)}0, {sreg(n)}1, v2.{sreg(n)}[{idx}]"
            else:
                asm = f"{op} v0.{A}, v1.{A}, v2.{sreg(n)}[{idx}]"
        elif kind == "cvt":
            form = rng.choice(["fcvtn", "fcvtl", "fcvtxn", "fcvtxn_s"])
            two = rng.choice([0, 1])
            if form == "fcvtl":
                frm = rng.choice([16, 32])
                to = frm * 2
                c2 = 64 // frm
                ins["q1"] = join([self.fpval(frm) for _ in range(128 // frm)], frm)
                src = lanes(ins["q1"], frm, 128 // frm)[c2 * two: c2 * two + c2]
                exp["q0"] = join([R.convert(x, frm, to, st.rmode, st) for x in src], to)
                asm = f"fcvtl{'2' if two else ''} v0.{arr(to, 1)}, v1.{arr(frm, two)}"
            elif form == "fcvtn":
                to = rng.choice([16, 32])
                frm = to * 2
                c2 = 64 // to
                ins["q1"] = join([self.fpval(frm) for _ in range(128 // frm)], frm)
                src = lanes(ins["q1"], frm, c2)
                outl = [R.convert(x, frm, to, st.rmode, st) for x in src]
                lo = join(outl, to)
                exp["q0"] = ((ins["q0"] & M64) | (lo << 64)) if two else lo
                asm = f"fcvtn{'2' if two else ''} v0.{arr(to, two)}, v1.{arr(frm, 1)}"
            elif form == "fcvtxn":
                ins["q1"] = join([self.fpval(64) for _ in range(2)], 64)
                outl = [R.convert(x, 64, 32, R.ODD, st) for x in lanes(ins["q1"], 64, 2)]
                lo = join(outl, 32)
                exp["q0"] = ((ins["q0"] & M64) | (lo << 64)) if two else lo
                asm = f"fcvtxn{'2' if two else ''} v0.{arr(32, two)}, v1.2d"
            else:
                ins["q1"] = self.fpval(64) | (self.rng.getrandbits(64) << 64)
                exp["q0"] = R.convert(ins["q1"] & M64, 64, 32, R.ODD, st)
                asm = "fcvtxn s0, d1"
        elif kind == "fixed":
            fb = rng.randrange(1, n + 1)
            scalar = rng.random() < 0.3
            use = 1 if scalar else cnt
            if rng.random() < 0.5:
                op = rng.choice(["fcvtzs", "fcvtzu"])
                exp["q0"] = join([R.to_fixed(x, n, fb, op == "fcvtzu", R.RZ, n, st) for x in a[:use]], n)
            else:
                op = rng.choice(["scvtf", "ucvtf"])
                ins["q1"] = self.vec_int(n)
                exp["q0"] = join([R.from_fixed(x, op == "scvtf", n, fb, n, st.rmode, st)
                                  for x in lanes(ins["q1"], n, use)], n)
            if scalar:
                asm = f"{op} {sreg(n)}0, {sreg(n)}1, #{fb}"
            else:
                asm = f"{op} v0.{A}, v1.{A}, #{fb}"
        else:  # across lanes, 4S only
            if n != 32:
                return
            ins["q1"] = self.vec_fp(32)
            a4 = lanes(ins["q1"], 32, 4)
            op = rng.choice(["fmaxv", "fminv", "fmaxnmv", "fminnmv"])
            mx = "max" in op
            num = "nm" in op
            f = lambda x, y: R.maxmin(x, y, 32, st, mx, num)  # noqa: E731
            exp["q0"] = f(f(a4[0], a4[1]), f(a4[2], a4[3]))
            asm = f"{op} s0, v1.4s"
        exp["fpsr"] = st.fpsr
        self.add(asm, ins, exp, CK_V | CK_FPSR)

    # ------------------------------------------------------------ integer SIMD
    def int_vec(self):
        rng = self.rng
        ins = self.base_inputs()
        esz = rng.choice([8, 16, 32, 64])
        q = 1 if esz == 64 else rng.choice([0, 1])
        cnt = (128 if q else 64) // esz
        A = arr(esz, q)
        ins["q1"], ins["q2"], ins["q3"] = self.vec_int(esz), self.vec_int(esz), self.vec_int(esz)
        ins["q0"] = self.vec_int(esz)
        a, b, d = lanes(ins["q1"], esz, cnt), lanes(ins["q2"], esz, cnt), lanes(ins["q0"], esz, cnt)
        M = R.mask(esz)
        state = {"qc": 0}

        def ssat(v, bits=esz):
            lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
            if v < lo or v > hi:
                state["qc"] = 1
                v = lo if v < lo else hi
            return v & R.mask(bits)

        def usat(v, bits=esz):
            if v < 0 or v > R.mask(bits):
                state["qc"] = 1
                v = 0 if v < 0 else R.mask(bits)
            return v

        S = lambda x: R.sx(x, esz)  # noqa: E731

        def shl_reg(x, y, uns, rnd, sat):
            sh = R.sx(y & 0xFF, 8)
            v = (x & M) if uns else S(x)
            if sh >= 0:
                r = v << sh
                if not sat:
                    return r & M
            else:
                rs = -sh
                r = (v + (1 << (rs - 1))) >> rs if rnd else v >> rs
                if not sat:
                    return r & M
            return usat(r) if uns else ssat(r)

        def sqdmulh(x, y, rnd):
            p = 2 * S(x) * S(y)
            if rnd:
                p += 1 << (esz - 1)
            return ssat(p >> esz)

        ops3 = {
            "add": lambda x, y, z: (x + y) & M, "sub": lambda x, y, z: (x - y) & M,
            "mul": lambda x, y, z: (x * y) & M, "mla": lambda x, y, z: (z + x * y) & M,
            "mls": lambda x, y, z: (z - x * y) & M,
            "sqadd": lambda x, y, z: ssat(S(x) + S(y)), "uqadd": lambda x, y, z: usat(x + y),
            "sqsub": lambda x, y, z: ssat(S(x) - S(y)), "uqsub": lambda x, y, z: usat(x - y),
            "shadd": lambda x, y, z: ((S(x) + S(y)) >> 1) & M, "uhadd": lambda x, y, z: ((x + y) >> 1) & M,
            "srhadd": lambda x, y, z: ((S(x) + S(y) + 1) >> 1) & M, "urhadd": lambda x, y, z: ((x + y + 1) >> 1) & M,
            "shsub": lambda x, y, z: ((S(x) - S(y)) >> 1) & M, "uhsub": lambda x, y, z: ((x - y) >> 1) & M,
            "cmgt": lambda x, y, z: M if S(x) > S(y) else 0, "cmhi": lambda x, y, z: M if x > y else 0,
            "cmge": lambda x, y, z: M if S(x) >= S(y) else 0, "cmhs": lambda x, y, z: M if x >= y else 0,
            "cmeq": lambda x, y, z: M if x == y else 0, "cmtst": lambda x, y, z: M if x & y else 0,
            "sshl": lambda x, y, z: shl_reg(x, y, 0, 0, 0), "ushl": lambda x, y, z: shl_reg(x, y, 1, 0, 0),
            "srshl": lambda x, y, z: shl_reg(x, y, 0, 1, 0), "urshl": lambda x, y, z: shl_reg(x, y, 1, 1, 0),
            "sqshl": lambda x, y, z: shl_reg(x, y, 0, 0, 1), "uqshl": lambda x, y, z: shl_reg(x, y, 1, 0, 1),
            "sqrshl": lambda x, y, z: shl_reg(x, y, 0, 1, 1), "uqrshl": lambda x, y, z: shl_reg(x, y, 1, 1, 1),
            "smax": lambda x, y, z: (max(S(x), S(y))) & M, "umax": lambda x, y, z: max(x, y),
            "smin": lambda x, y, z: (min(S(x), S(y))) & M, "umin": lambda x, y, z: min(x, y),
            "sabd": lambda x, y, z: abs(S(x) - S(y)) & M, "uabd": lambda x, y, z: abs(x - y),
            "saba": lambda x, y, z: (z + abs(S(x) - S(y))) & M, "uaba": lambda x, y, z: (z + abs(x - y)) & M,
            "sqdmulh": lambda x, y, z: sqdmulh(x, y, 0), "sqrdmulh": lambda x, y, z: sqdmulh(x, y, 1),
        }
        no64 = {"mul", "mla", "mls", "shadd", "uhadd", "srhadd", "urhadd", "shsub", "uhsub", "smax", "umax", "smin",
                "umin", "sabd", "uabd", "saba", "uaba", "sqdmulh", "sqrdmulh"}
        kind = rng.choice(["3same"] * 5 + ["pair", "logic", "pmul"])
        if kind == "3same":
            op = rng.choice(list(ops3))
            if esz == 64 and op in no64:
                return
            if op in ("sqdmulh", "sqrdmulh") and esz == 8:
                return
            if op in ("sshl", "ushl", "srshl", "urshl", "sqshl", "uqshl", "sqrshl", "uqrshl"):
                # keep shift amounts interesting
                b = [(rng.randrange(-esz - 2, esz + 3) & 0xFF) | (rng.getrandbits(esz) & ~0xFF & M) for _ in b]
                ins["q2"] = join(b, esz) | ((ins["q2"] & ~R.mask(esz * cnt)) if not q else 0)
            out = [ops3[op](x, y, z) for x, y, z in zip(a, b, d)]
            exp_v = join(out, esz)
            asm = f"{op} v0.{A}, v1.{A}, v2.{A}"
        elif kind == "pair":
            op = rng.choice(["addp", "smaxp", "umaxp", "sminp", "uminp"])
            if esz == 64 and op != "addp":
                return
            cat = a + b
            f = {"addp": lambda x, y: (x + y) & M, "smaxp": lambda x, y: max(S(x), S(y)) & M,
                 "umaxp": max, "sminp": lambda x, y: min(S(x), S(y)) & M, "uminp": min}[op]
            exp_v = join([f(cat[2 * i], cat[2 * i + 1]) for i in range(cnt)], esz)
            asm = f"{op} v0.{A}, v1.{A}, v2.{A}"
        elif kind == "logic":
            op = rng.choice(["and", "bic", "orr", "orn", "eor", "bsl", "bit", "bif"])
            A8 = "16b" if q else "8b"
            nb = 16 if q else 8
            x, y, z = ins["q1"], ins["q2"], ins["q0"]
            m = R.mask(nb * 8)
            v = {"and": x & y, "bic": x & ~y, "orr": x | y, "orn": x | ~y, "eor": x ^ y,
                 "bsl": (z & x) | (~z & y), "bit": (y & x) | (~y & z), "bif": (~y & x) | (y & z)}[op] & m
            exp_v = v
            asm = f"{op} v0.{A8}, v1.{A8}, v2.{A8}"
            cnt, esz = nb, 8
        else:
            if esz != 8:
                return
            def pm(x, y):
                r = 0
                for i in range(8):
                    if (x >> i) & 1:
                        r ^= y << i
                return r & 0xFF
            exp_v = join([pm(x, y) for x, y in zip(a, b)], 8)
            asm = f"pmul v0.{A}, v1.{A}, v2.{A}"
        fpsr = R.QC if state["qc"] else 0
        self.add(asm, ins, {"q0": exp_v, "x0": ins["x0"], "fpsr": fpsr, "nzcv": ins["nzcv"]}, CK_V | CK_FPSR)

    def int_misc(self):
        rng = self.rng
        ins = self.base_inputs()
        esz = rng.choice([8, 16, 32, 64])
        q = 1 if esz == 64 else rng.choice([0, 1])
        cnt = (128 if q else 64) // esz
        A = arr(esz, q)
        ins["q1"] = self.vec_int(esz)
        a = lanes(ins["q1"], esz, cnt)
        d = lanes(ins["q0"], esz, cnt)
        M = R.mask(esz)
        S = lambda x: R.sx(x, esz)  # noqa: E731
        st = {"qc": 0}

        def ssat(v, bits=esz):
            lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
            if v < lo or v > hi:
                st["qc"] = 1
                v = lo if v < lo else hi
            return v & R.mask(bits)

        def usat(v, bits=esz):
            if v < 0 or v > R.mask(bits):
                st["qc"] = 1
                v = 0 if v < 0 else R.mask(bits)
            return v

        kind = rng.choice(["simple"] * 4 + ["rev", "cnt", "narrow", "long", "pairl", "across", "shift", "shift",
                                             "narrowshift", "longshift", "modimm", "copy", "perm", "ext", "tbl",
                                             "diff", "diff", "indexed", "scalar"])
        exp = {"x0": ins["x0"], "nzcv": ins["nzcv"]}
        check = CK_V | CK_FPSR
        if kind == "simple":
            op = rng.choice(["abs", "neg", "sqabs", "sqneg", "cls", "clz", "cmgt0", "cmge0", "cmeq0", "cmle0", "cmlt0",
                             "suqadd", "usqadd"])
            if op in ("cls", "clz") and esz == 64:
                return
            def f(x, z):
                if op == "abs":
                    return abs(S(x)) & M
                if op == "neg":
                    return (-S(x)) & M
                if op == "sqabs":
                    return ssat(abs(S(x)))
                if op == "sqneg":
                    return ssat(-S(x))
                if op == "cls":
                    t = ((x >> 1) ^ x) & R.mask(esz - 1)
                    return (esz - 1) - t.bit_length()
                if op == "clz":
                    return esz - x.bit_length()
                if op == "suqadd":
                    return ssat(S(z) + x)
                if op == "usqadd":
                    return usat(z + S(x))
                c = {"cmgt0": S(x) > 0, "cmge0": S(x) >= 0, "cmeq0": x == 0, "cmle0": S(x) <= 0, "cmlt0": S(x) < 0}[op]
                return M if c else 0
            v = join([f(x, z) for x, z in zip(a, d)], esz)
            name = op[:-1] if op.endswith("0") else op
            asm = f"{name} v0.{A}, v1.{A}" + (", #0" if op.endswith("0") else "")
        elif kind == "rev":
            op = rng.choice(["rev64", "rev32", "rev16"])
            cont = {"rev64": 64, "rev32": 32, "rev16": 16}[op]
            if esz >= cont:
                return
            per = cont // esz
            out = []
            for base in range(0, cnt, per):
                out += list(reversed(a[base:base + per]))
            v = join(out, esz)
            asm = f"{op} v0.{A}, v1.{A}"
        elif kind == "cnt":
            op = rng.choice(["cnt", "not", "rbit"])
            A8 = "16b" if q else "8b"
            nb = 16 if q else 8
            bs = lanes(ins["q1"], 8, nb)
            if op == "cnt":
                v = join([bin(x).count("1") for x in bs], 8)
            elif op == "not":
                v = join([~x & 0xFF for x in bs], 8)
            else:
                v = join([int(f"{x:08b}"[::-1], 2) for x in bs], 8)
            asm = f"{op} v0.{A8}, v1.{A8}"
        elif kind == "narrow":
            if esz == 64:
                return
            op = rng.choice(["xtn", "sqxtn", "uqxtn", "sqxtun"])
            two = rng.choice([0, 1])
            ncnt = 64 // esz
            ins["q1"] = self.vec_int(2 * esz)
            w = lanes(ins["q1"], 2 * esz, ncnt)
            outl = []
            for x in w:
                if op == "xtn":
                    outl.append(x & M)
                elif op == "sqxtn":
                    outl.append(ssat(R.sx(x, 2 * esz)))
                elif op == "uqxtn":
                    outl.append(usat(x))
                else:
                    outl.append(usat(R.sx(x, 2 * esz)))
            lo = join(outl, esz)
            v = ((ins["q0"] & M64) | (lo << 64)) if two else lo
            asm = f"{op}{'2' if two else ''} v0.{arr(esz, two)}, v1.{arr(2 * esz, 1)}"
        elif kind == "long":
            if esz == 64:
                return
            op = rng.choice(["sxtl", "uxtl", "shll"])
            two = rng.choice([0, 1])
            ncnt = 64 // esz
            src = lanes(ins["q1"], esz, 128 // esz)[ncnt * two: ncnt * two + ncnt]
            if op == "sxtl":
                v = join([R.sx(x, esz) & R.mask(2 * esz) for x in src], 2 * esz)
            elif op == "uxtl":
                v = join(src, 2 * esz)
            else:
                v = join([x << esz for x in src], 2 * esz)
            if op == "shll":
                asm = f"shll{'2' if two else ''} v0.{arr(2 * esz, 1)}, v1.{arr(esz, two)}, #{esz}"
            else:
                asm = f"{op}{'2' if two else ''} v0.{arr(2 * esz, 1)}, v1.{arr(esz, two)}"
        elif kind == "pairl":
            if esz == 64:
                return
            op = rng.choice(["saddlp", "uaddlp", "sadalp", "uadalp"])
            wcnt = cnt // 2
            dl = lanes(ins["q0"], 2 * esz, wcnt)
            out = []
            for i in range(wcnt):
                x, y = a[2 * i], a[2 * i + 1]
                s = (x + y) if op[0] == "u" else (S(x) + S(y))
                if "dal" in op:
                    s += dl[i]
                out.append(s & R.mask(2 * esz))
            v = join(out, 2 * esz)
            asm = f"{op} v0.{arr(2 * esz, q)}, v1.{A}"
        elif kind == "across":
            if esz == 64 or (esz == 32 and not q):
                return
            op = rng.choice(["addv", "saddlv", "uaddlv", "smaxv", "umaxv", "sminv", "uminv"])
            if op == "addv":
                v = sum(a) & M
                r = sreg(esz)
            elif op in ("saddlv", "uaddlv"):
                v = (sum(a) if op[0] == "u" else sum(S(x) for x in a)) & R.mask(2 * esz)
                r = sreg(2 * esz)
            else:
                if op[0] == "s":
                    t = (max if "max" in op else min)(S(x) for x in a)
                    v = t & M
                else:
                    v = (max if "max" in op else min)(a)
                r = sreg(esz)
            asm = f"{op} {r}0, v1.{A}"
        elif kind == "shift":
            op = rng.choice(["shl", "sshr", "ushr", "srshr", "urshr", "ssra", "usra", "srsra", "ursra", "sri", "sli",
                             "sqshl", "uqshl", "sqshlu"])
            left = op in ("shl", "sli", "sqshl", "uqshl", "sqshlu")
            sh = rng.randrange(0, esz) if left else rng.randrange(1, esz + 1)
            out = []
            for x, z in zip(a, d):
                if op == "shl":
                    r = (x << sh) & M
                elif op == "sli":
                    r = ((x << sh) & M) | (z & R.mask(sh))
                elif op == "sqshl":
                    r = ssat(S(x) << sh)
                elif op == "uqshl":
                    r = usat(x << sh)
                elif op == "sqshlu":
                    r = usat(S(x) << sh)
                elif op == "sri":
                    keep = M if sh >= esz else (M & ~(M >> sh))
                    r = (z & keep) | (x >> sh if sh < esz else 0)
                else:
                    sv = S(x) if op[0] == "s" else x
                    if "r" in op[1:3] and op not in ("ssra", "usra"):
                        sv += 1 << (sh - 1)
                    r = (sv >> sh) & M
                    if op.endswith("sra"):
                        r = (r + z) & M
                out.append(r)
            v = join(out, esz)
            asm = f"{op} v0.{A}, v1.{A}, #{sh}"
        elif kind == "narrowshift":
            if esz == 64:
                return
            op = rng.choice(["shrn", "rshrn", "sqshrn", "sqrshrn", "uqshrn", "uqrshrn", "sqshrun", "sqrshrun"])
            two = rng.choice([0, 1])
            ncnt = 64 // esz
            sh = rng.randrange(1, esz + 1)
            ins["q1"] = self.vec_int(2 * esz)
            w = lanes(ins["q1"], 2 * esz, ncnt)
            outl = []
            for x in w:
                sv = x if op in ("shrn", "rshrn", "uqshrn", "uqrshrn") else R.sx(x, 2 * esz)
                if "rshr" in op:
                    sv += 1 << (sh - 1)
                sv >>= sh
                if op in ("shrn", "rshrn"):
                    outl.append(sv & M)
                elif op.startswith("uq") or op.endswith("un"):
                    outl.append(usat(sv))
                else:
                    outl.append(ssat(sv))
            lo = join(outl, esz)
            v = ((ins["q0"] & M64) | (lo << 64)) if two else lo
            asm = f"{op}{'2' if two else ''} v0.{arr(esz, two)}, v1.{arr(2 * esz, 1)}, #{sh}"
        elif kind == "longshift":
            if esz == 64:
                return
            op = rng.choice(["sshll", "ushll"])
            two = rng.choice([0, 1])
            ncnt = 64 // esz
            sh = rng.randrange(0, esz)
            src = lanes(ins["q1"], esz, 128 // esz)[ncnt * two: ncnt * two + ncnt]
            v = join([((R.sx(x, esz) if op[0] == "s" else x) << sh) & R.mask(2 * esz) for x in src], 2 * esz)
            asm = f"{op}{'2' if two else ''} v0.{arr(2 * esz, 1)}, v1.{arr(esz, two)}, #{sh}"
        elif kind == "modimm":
            form = rng.choice(["movi8", "movi16", "movi32", "msl", "movi64", "mvni16", "mvni32", "orr32", "bic16",
                               "fmov4s", "fmov2d"])
            imm8 = rng.randrange(256)
            qq = rng.choice([0, 1])
            nb = 128 if qq else 64
            rep = lambda val, w: join([val] * (nb // w), w)  # noqa: E731
            if form == "movi8":
                v = rep(imm8, 8)
                asm = f"movi v0.{arr(8, qq)}, #{imm8}"
            elif form == "movi16":
                sh = rng.choice([0, 8])
                v = rep(imm8 << sh, 16)
                asm = f"movi v0.{arr(16, qq)}, #{imm8}, lsl #{sh}"
            elif form == "movi32":
                sh = rng.choice([0, 8, 16, 24])
                v = rep(imm8 << sh, 32)
                asm = f"movi v0.{arr(32, qq)}, #{imm8}, lsl #{sh}"
            elif form == "msl":
                sh = rng.choice([8, 16])
                v = rep((imm8 << sh) | R.mask(sh), 32)
                asm = f"movi v0.{arr(32, qq)}, #{imm8}, msl #{sh}"
            elif form == "movi64":
                val = 0
                for i in range(8):
                    if (imm8 >> i) & 1:
                        val |= 0xFF << (8 * i)
                v = val if not qq else (val | (val << 64))
                asm = f"movi {'v0.2d' if qq else 'd0'}, #{val:#x}"
                nb = 128 if qq else 64
            elif form == "mvni16":
                sh = rng.choice([0, 8])
                v = rep(~(imm8 << sh) & 0xFFFF, 16)
                asm = f"mvni v0.{arr(16, qq)}, #{imm8}, lsl #{sh}"
            elif form == "mvni32":
                sh = rng.choice([0, 8, 16, 24])
                v = rep(~(imm8 << sh) & 0xFFFFFFFF, 32)
                asm = f"mvni v0.{arr(32, qq)}, #{imm8}, lsl #{sh}"
            elif form == "orr32":
                sh = rng.choice([0, 8, 16, 24])
                v = (ins["q0"] & R.mask(nb)) | rep(imm8 << sh, 32)
                asm = f"orr v0.{arr(32, qq)}, #{imm8}, lsl #{sh}"
            elif form == "bic16":
                sh = rng.choice([0, 8])
                v = (ins["q0"] & R.mask(nb)) & ~rep(imm8 << sh, 16)
                asm = f"bic v0.{arr(16, qq)}, #{imm8}, lsl #{sh}"
            else:
                import struct
                w = 32 if form == "fmov4s" else 64
                if w == 64:
                    qq, nb = 1, 128
                bits = R.fp_imm(imm8, w)
                val = struct.unpack("<f" if w == 32 else "<d", struct.pack("<I" if w == 32 else "<Q", bits))[0]
                v = rep(bits, w)
                asm = f"fmov v0.{arr(w, qq)}, #{val!r}"
            v &= R.mask(nb)
        elif kind == "copy":
            form = rng.choice(["dup_e", "dup_g", "ins_g", "ins_e", "umov", "smov", "dup_s"])
            idx = rng.randrange(128 // esz)
            el = lanes(ins["q1"], esz, 128 // esz)[idx]
            if form == "dup_e":
                v = join([el] * cnt, esz)
                asm = f"dup v0.{A}, v1.{sreg(esz)}[{idx}]"
            elif form == "dup_g":
                v = join([ins["x1"] & M] * cnt, esz)
                asm = f"dup v0.{A}, {'x' if esz == 64 else 'w'}1"
            elif form == "ins_g":
                l0 = lanes(ins["q0"], esz, 128 // esz)
                l0[idx] = ins["x1"] & M
                v = join(l0, esz)
                asm = f"mov v0.{sreg(esz)}[{idx}], {'x' if esz == 64 else 'w'}1"
            elif form == "ins_e":
                l0 = lanes(ins["q0"], esz, 128 // esz)
                i2 = rng.randrange(128 // esz)
                l0[idx] = lanes(ins["q1"], esz, 128 // esz)[i2]
                v = join(l0, esz)
                asm = f"mov v0.{sreg(esz)}[{idx}], v1.{sreg(esz)}[{i2}]"
            elif form == "dup_s":
                v = el
                asm = f"mov {sreg(esz)}0, v1.{sreg(esz)}[{idx}]"
            elif form == "umov":
                exp["x0"] = el
                v = ins["q0"]
                check = CK_V | CK_X
                asm = f"umov {'x' if esz == 64 else 'w'}0, v1.{sreg(esz)}[{idx}]"
            else:
                if esz == 64:
                    return
                wide = 64 if esz == 32 else rng.choice([32, 64])
                exp["x0"] = R.sx(el, esz) & R.mask(wide)
                v = ins["q0"]
                check = CK_V | CK_X
                asm = f"smov {'x' if wide == 64 else 'w'}0, v1.{sreg(esz)}[{idx}]"
        elif kind == "perm":
            op = rng.choice(["zip1", "zip2", "uzp1", "uzp2", "trn1", "trn2"])
            b = lanes(ins["q2"], esz, cnt)
            half = cnt // 2
            part = 1 if op.endswith("2") else 0
            out = [0] * cnt
            if op.startswith("zip"):
                for i in range(half):
                    out[2 * i] = a[i + part * half]
                    out[2 * i + 1] = b[i + part * half]
            elif op.startswith("uzp"):
                for i in range(half):
                    out[i] = a[2 * i + part]
                    out[i + half] = b[2 * i + part]
            else:
                for i in range(half):
                    out[2 * i] = a[2 * i + part]
                    out[2 * i + 1] = b[2 * i + part]
            v = join(out, esz)
            asm = f"{op} v0.{A}, v1.{A}, v2.{A}"
        elif kind == "ext":
            nb = 16 if q else 8
            imm = rng.randrange(nb)
            cat = (ins["q1"] & R.mask(8 * nb)) | ((ins["q2"] & R.mask(8 * nb)) << (8 * nb))
            v = (cat >> (8 * imm)) & R.mask(8 * nb)
            A8 = "16b" if q else "8b"
            asm = f"ext v0.{A8}, v1.{A8}, v2.{A8}, #{imm}"
        elif kind == "tbl":
            # tables v1..v2 (len 2) or v1 (len 1); indices in v3
            ln = rng.choice([1, 2])
            tbx = rng.random() < 0.5
            nb = 16 if q else 8
            idx = [rng.randrange(0, 40) for _ in range(nb)]
            ins["q3"] = join(idx, 8) | (rng.getrandbits(64) << 64 if not q else 0)
            table = lanes(ins["q1"], 8, 16) + (lanes(ins["q2"], 8, 16) if ln == 2 else [])
            d8 = lanes(ins["q0"], 8, 16)
            out = [table[i] if i < len(table) else (d8[k] if tbx else 0) for k, i in enumerate(idx)]
            v = join(out, 8)
            A8 = "16b" if q else "8b"
            regs = "{v1.16b}" if ln == 1 else "{v1.16b, v2.16b}"
            asm = f"{'tbx' if tbx else 'tbl'} v0.{A8}, {regs}, v3.{A8}"
        elif kind == "diff":
            if esz == 64:
                return
            op = rng.choice(["saddl", "uaddl", "ssubl", "usubl", "saddw", "uaddw", "ssubw", "usubw", "addhn", "raddhn",
                             "subhn", "rsubhn", "sabal", "uabal", "sabdl", "uabdl", "smlal", "umlal", "smlsl",
                             "umlsl", "smull", "umull", "sqdmull", "sqdmlal", "sqdmlsl", "pmull"])
            if op.startswith("sqd") and esz == 8:
                return
            if op == "pmull" and esz != 8:
                return
            two = rng.choice([0, 1])
            ncnt = 64 // esz
            W = 2 * esz
            if op in ("addhn", "raddhn", "subhn", "rsubhn"):
                ins["q1"], ins["q2"] = self.vec_int(W), self.vec_int(W)
                x, y = lanes(ins["q1"], W, ncnt), lanes(ins["q2"], W, ncnt)
                outl = []
                for p, r2 in zip(x, y):
                    s = p + r2 if "add" in op else p - r2
                    if op.startswith("r"):
                        s += 1 << (esz - 1)
                    outl.append((s >> esz) & M)
                lo = join(outl, esz)
                v = ((ins["q0"] & M64) | (lo << 64)) if two else lo
                asm = f"{op}{'2' if two else ''} v0.{arr(esz, two)}, v1.{arr(W, 1)}, v2.{arr(W, 1)}"
            else:
                wide1 = op.endswith("w")
                if wide1:
                    ins["q1"] = self.vec_int(W)
                n1 = lanes(ins["q1"], W, ncnt) if wide1 else lanes(ins["q1"], esz, 128 // esz)[ncnt * two:ncnt * two + ncnt]
                m1 = lanes(ins["q2"], esz, 128 // esz)[ncnt * two:ncnt * two + ncnt]
                acc = lanes(ins["q0"], W, ncnt)
                uns = op[0] == "u"
                out = []
                for x, y, z in zip(n1, m1, acc):
                    X = (x if uns else R.sx(x, W)) if wide1 else (x if uns else R.sx(x, esz))
                    Y = y if uns else R.sx(y, esz)
                    Z = z if uns else R.sx(z, W)
                    base = op[1:] if op[0] in "su" else op
                    if op == "pmull":
                        r = 0
                        for i in range(8):
                            if (x >> i) & 1:
                                r ^= y << i
                    elif op.startswith("sqd"):
                        p = ssat(2 * X * Y, W)
                        p = R.sx(p, W)
                        if op == "sqdmull":
                            r = p
                        elif op == "sqdmlal":
                            r = ssat(Z + p, W)
                        else:
                            r = ssat(Z - p, W)
                    elif base in ("addl", "addw"):
                        r = X + Y
                    elif base in ("subl", "subw"):
                        r = X - Y
                    elif base == "abal":
                        r = Z + abs(X - Y)
                    elif base == "abdl":
                        r = abs(X - Y)
                    elif base == "mlal":
                        r = Z + X * Y
                    elif base == "mlsl":
                        r = Z - X * Y
                    else:
                        r = X * Y
                    out.append(r & R.mask(W))
                v = join(out, W)
                src1 = arr(W, 1) if wide1 else arr(esz, two)
                name = op + ("2" if two else "")
                asm = f"{name} v0.{arr(W, 1)}, v1.{src1}, v2.{arr(esz, two)}"
        elif kind == "indexed":
            if esz in (8, 64):
                return
            op = rng.choice(["mul", "mla", "mls", "sqdmulh", "sqrdmulh", "smull", "umull", "smlal", "umlal", "smlsl",
                             "umlsl", "sqdmull", "sqdmlal", "sqdmlsl"])
            maxidx = 8 if esz == 16 else 4
            idx = rng.randrange(maxidx)
            mreg = rng.randrange(0, 16) if esz == 16 else 2
            if esz == 16:
                mreg = 2
            e2 = lanes(ins["q2"], esz, 128 // esz)[idx]
            if op in ("mul", "mla", "mls", "sqdmulh", "sqrdmulh"):
                out = []
                for x, z in zip(a, d):
                    if op == "mul":
                        r = (x * e2) & M
                    elif op == "mla":
                        r = (z + x * e2) & M
                    elif op == "mls":
                        r = (z - x * e2) & M
                    else:
                        p = 2 * S(x) * S(e2)
                        if op == "sqrdmulh":
                            p += 1 << (esz - 1)
                        r = ssat(p >> esz)
                    out.append(r)
                v = join(out, esz)
                asm = f"{op} v0.{A}, v1.{A}, v{mreg}.{sreg(esz)}[{idx}]"
            else:
                two = rng.choice([0, 1])
                ncnt = 64 // esz
                W = 2 * esz
                src = lanes(ins["q1"], esz, 128 // esz)[ncnt * two:ncnt * two + ncnt]
                acc = lanes(ins["q0"], W, ncnt)
                uns = op[0] == "u"
                out = []
                for x, z in zip(src, acc):
                    X = x if uns else S(x)
                    Y = e2 if uns else S(e2)
                    Z = z if uns else R.sx(z, W)
                    if op.startswith("sqd"):
                        p = R.sx(ssat(2 * X * Y, W), W)
                        r = p if op == "sqdmull" else ssat(Z + p if op == "sqdmlal" else Z - p, W)
                    elif op.endswith("mull"):
                        r = X * Y
                    elif op.endswith("mlal"):
                        r = Z + X * Y
                    else:
                        r = Z - X * Y
                    out.append(r & R.mask(W))
                v = join(out, W)
                asm = f"{op}{'2' if two else ''} v0.{arr(W, 1)}, v1.{arr(esz, two)}, v{mreg}.{sreg(esz)}[{idx}]"
        else:  # scalar integer forms
            op = rng.choice(["add", "sub", "cmgt", "cmeq", "cmtst", "sshl", "ushl", "sqadd", "uqsub", "abs", "neg",
                             "sshr", "ushr", "shl", "ursra", "addp", "sqxtn", "uqxtn", "sqshl_imm", "sqrdmulh"])
            sz = 64 if op in ("add", "sub", "cmgt", "cmeq", "cmtst", "sshl", "ushl", "abs", "neg", "sshr", "ushr",
                              "shl", "ursra", "addp") else rng.choice([8, 16, 32, 64])
            if op in ("sqxtn", "uqxtn") and sz == 64:
                sz = 32
            if op == "sqrdmulh" and sz in (8, 64):
                sz = 16
            Ms = R.mask(sz)
            x = ins["q1"] & Ms
            y = ins["q2"] & Ms
            z = ins["q0"] & Ms
            Sx = R.sx(x, sz)
            Sy = R.sx(y, sz)
            r = sreg(sz)

            def ss(v, bits=sz):
                lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
                if v < lo or v > hi:
                    st["qc"] = 1
                    v = lo if v < lo else hi
                return v & R.mask(bits)

            def us(v, bits=sz):
                if v < 0 or v > R.mask(bits):
                    st["qc"] = 1
                    v = 0 if v < 0 else R.mask(bits)
                return v
            if op == "add":
                v, asm = (x + y) & Ms, "add d0, d1, d2"
            elif op == "sub":
                v, asm = (x - y) & Ms, "sub d0, d1, d2"
            elif op == "cmgt":
                v, asm = (Ms if Sx > Sy else 0), "cmgt d0, d1, d2"
            elif op == "cmeq":
                v, asm = (Ms if x == y else 0), "cmeq d0, d1, d2"
            elif op == "cmtst":
                v, asm = (Ms if x & y else 0), "cmtst d0, d1, d2"
            elif op in ("sshl", "ushl"):
                y = (rng.randrange(-70, 70) & 0xFF) | (y & ~0xFF)
                ins["q2"] = (ins["q2"] & ~Ms) | y
                sh = R.sx(y & 0xFF, 8)
                val = Sx if op == "sshl" else x
                v = ((val << sh) if sh >= 0 else (val >> -sh)) & Ms
                asm = f"{op} d0, d1, d2"
            elif op == "sqadd":
                v, asm = ss(Sx + Sy), f"sqadd {r}0, {r}1, {r}2"
            elif op == "uqsub":
                v, asm = us(x - y), f"uqsub {r}0, {r}1, {r}2"
            elif op == "abs":
                v, asm = abs(Sx) & Ms, "abs d0, d1"
            elif op == "neg":
                v, asm = (-Sx) & Ms, "neg d0, d1"
            elif op in ("sshr", "ushr", "ursra"):
                sh = rng.randrange(1, 65)
                val = Sx if op == "sshr" else x
                if op == "ursra":
                    val += 1 << (sh - 1)
                v = (val >> sh) & Ms
                if op == "ursra":
                    v = (v + z) & Ms
                asm = f"{op} d0, d1, #{sh}"
            elif op == "shl":
                sh = rng.randrange(0, 64)
                v, asm = (x << sh) & Ms, f"shl d0, d1, #{sh}"
            elif op == "addp":
                v, asm = ((ins["q1"] & M64) + (ins["q1"] >> 64)) & M64, "addp d0, v1.2d"
            elif op in ("sqxtn", "uqxtn"):
                w = ins["q1"] & R.mask(2 * sz)
                if op == "sqxtn":
                    v = ss(R.sx(w, 2 * sz))
                else:
                    v = us(w)
                asm = f"{op} {r}0, {sreg(2 * sz)}1"
            elif op == "sqshl_imm":
                sh = rng.randrange(0, sz)
                v, asm = ss(Sx << sh), f"sqshl {r}0, {r}1, #{sh}"
            else:
                p = 2 * Sx * Sy + (1 << (sz - 1))
                v, asm = ss(p >> sz), f"sqrdmulh {r}0, {r}1, {r}2"
        exp["q0"] = v
        exp["fpsr"] = R.QC if st["qc"] else 0
        self.add(asm, ins, exp, check)

    def ldst(self):
        """SIMD load/store instructions run against a scratch buffer at x1 (set by the harness)."""
        rng = self.rng
        ins = self.base_inputs()
        ins["q1"], ins["q2"], ins["q3"] = rng.getrandbits(128), rng.getrandbits(128), rng.getrandbits(128)
        # The harness puts the buffer address in x1 and a fixed 64-byte pattern in it.
        buf = bytes((i * 37 + 11) & 0xFF for i in range(64))
        off = rng.randrange(0, 16)
        ins["x1"] = off      # harness adds the buffer base
        form = rng.choice(["ldr_q", "ldr_d", "ldr_s", "ldr_h", "ldr_b", "ld1_2s", "ld1r", "ld1_lane", "ld2", "ld1x2",
                           "ldp_q", "ld3_lane", "ld2_lane", "ld4r", "ld4"])
        exp = {"x0": ins["x0"], "nzcv": ins["nzcv"], "fpsr": 0}
        rd = lambda o, n: int.from_bytes(buf[o:o + n], "little")  # noqa: E731
        if form.startswith("ldr_") and form != "ldr_q":
            n = {"d": 8, "s": 4, "h": 2, "b": 1}[form[-1]]
            exp["q0"] = rd(off, n)
            asm = f"ldr {form[-1]}0, [x1]"
        elif form == "ldr_q":
            exp["q0"] = rd(off, 16)
            asm = "ldr q0, [x1]"
        elif form == "ld1_2s":
            exp["q0"] = rd(off, 8)
            asm = "ld1 {v0.2s}, [x1]"
        elif form == "ld1r":
            esz = rng.choice([1, 2, 4, 8])
            q = rng.choice([0, 1])
            e = rd(off, esz)
            exp["q0"] = join([e] * ((16 if q else 8) // esz), esz * 8)
            asm = f"ld1r {{v0.{arr(esz * 8, q)}}}, [x1]"
        elif form == "ld1_lane":
            esz = rng.choice([1, 2, 4, 8])
            idx = rng.randrange(16 // esz)
            l0 = lanes(ins["q0"], esz * 8, 16 // esz)
            l0[idx] = rd(off, esz)
            exp["q0"] = join(l0, esz * 8)
            asm = f"ld1 {{v0.{sreg(esz * 8)}}}[{idx}], [x1]"
        elif form == "ld2":
            exp["q0"] = join([rd(off + 4 * i, 2) for i in range(4)], 16)
            exp["q1"] = join([rd(off + 4 * i + 2, 2) for i in range(4)], 16)
            asm = "ld2 {v0.4h, v1.4h}, [x1]"
        elif form == "ld1x2":
            exp["q0"] = rd(off, 16)
            exp["q1"] = rd(off + 16, 16)
            asm = "ld1 {v0.16b, v1.16b}, [x1]"
        elif form == "ldp_q":
            exp["q0"] = rd(off, 16)
            exp["q1"] = rd(off + 16, 16)
            asm = "ldp q0, q1, [x1]"
        elif form == "ld3_lane":
            esz = rng.choice([1, 2, 4, 8])
            idx = rng.randrange(16 // esz)
            for k, r in enumerate(("q0", "q1", "q2")):
                l = lanes(ins[r], esz * 8, 16 // esz)
                l[idx] = rd(off + k * esz, esz)
                exp[r] = join(l, esz * 8)
            asm = f"ld3 {{v0.{sreg(esz * 8)}, v1.{sreg(esz * 8)}, v2.{sreg(esz * 8)}}}[{idx}], [x1]"
        elif form == "ld2_lane":
            esz = rng.choice([1, 2, 4, 8])
            idx = rng.randrange(16 // esz)
            for k, r in enumerate(("q0", "q1")):
                l = lanes(ins[r], esz * 8, 16 // esz)
                l[idx] = rd(off + k * esz, esz)
                exp[r] = join(l, esz * 8)
            asm = f"ld2 {{v0.{sreg(esz * 8)}, v1.{sreg(esz * 8)}}}[{idx}], [x1]"
        elif form == "ld4r":
            esz = rng.choice([1, 2, 4, 8])
            q = rng.choice([0, 1]) if esz < 8 else 1
            for k, r in enumerate(("q0", "q1", "q2", "q3")):
                exp[r] = join([rd(off + k * esz, esz)] * ((16 if q else 8) // esz), esz * 8)
            a = arr(esz * 8, q)
            asm = f"ld4r {{v0.{a}, v1.{a}, v2.{a}, v3.{a}}}, [x1]"
        else:
            for r in range(4):
                exp[f"q{r}"] = join([rd(off + 4 * i + r, 1) for i in range(8)], 8)
            asm = "ld4 {v0.8b, v1.8b, v2.8b, v3.8b}, [x1]"
        ins["ldst"] = 1
        self.add(asm, ins, exp, CK_V)

    def known(self):
        """Hand-checked values from the ARM ARM (independent of simd_ref)."""
        cases = [
            ("frecpe s0, s1", 0x3F800000, 0x3F7F8000),     # 1.0 -> 0.998046875
            ("frsqrte s0, s1", 0x3F800000, 0x3F7F8000),    # 1.0 -> 0.998046875
            ("frsqrte s0, s1", 0x40800000, 0x3EFF8000),    # 4.0 -> 0.4990234375
            ("frsqrte s0, s1", 0x40000000, 0x3F348000),    # 2.0 -> 0.705078125
            ("fadd s0, s1, s2", 0x3F800000, 0x40000000),   # 1 + 1 = 2 (with q2=1.0)
        ]
        for asm, a, e in cases:
            ins = self.base_inputs()
            ins["q1"] = a
            ins["q2"] = 0x3F800000
            ins["fpcr"] = 0
            self.add(asm, ins, {"q0": e, "x0": ins["x0"], "fpsr": 0, "nzcv": ins["nzcv"]}, CK_V | CK_FPSR)

    def build(self, count):
        self.known()
        funcs = [self.scalar_fp] * 4 + [self.vector_fp] * 3 + [self.int_vec] * 3 + [self.int_misc] * 4 + [self.ldst]
        while len(self.cases) < count:
            self.rng.choice(funcs)()

    def assemble(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "a.s")
            with open(src, "w") as f:
                for asm, *_ in self.cases:
                    f.write(asm + "\n")
            p = subprocess.run(["llvm-mc", "-triple=aarch64", "-mattr=+v8.2a,+fullfp16,+rcpc,+crc",
                                "-show-encoding", src], capture_output=True, text=True)
            if p.returncode != 0:
                sys.stderr.write(p.stderr[:4000])
                raise SystemExit("llvm-mc failed")
        enc = re.findall(r"encoding: \[0x([0-9a-f]{2}),0x([0-9a-f]{2}),0x([0-9a-f]{2}),0x([0-9a-f]{2})\]", p.stdout)
        assert len(enc) == len(self.cases), (len(enc), len(self.cases))
        return [int(b3 + b2 + b1 + b0, 16) for b0, b1, b2, b3 in enc]

    def emit(self, path):
        words = self.assemble()
        out = ["/* generated by tests/gen_simd.py - do not edit */", "    .section .rodata", "    .balign 16",
               "    .global simd_cases", "simd_cases:"]
        for (asm, ins, exp, check), w in zip(self.cases, words):
            ld = ins.get("ldst", 0)
            def q(v):
                v &= M128
                return [v & M64, v >> 64]
            vals = [w | ((check | (16 if ld else 0)) << 32), 0] + q(ins["q0"]) + q(ins["q1"]) + q(ins["q2"]) + \
                q(ins["q3"]) + [ins["x0"] & M64, ins["x1"] & M64, ins["fpcr"], ins["nzcv"]] + q(exp["q0"]) + \
                [exp["x0"] & M64, exp["fpsr"], exp["nzcv"], 0] + q(exp["q1"]) + q(exp["q2"]) + q(exp["q3"])
            out.append("    .quad " + ", ".join(f"{v:#x}" for v in vals))
        out += ["    .global simd_case_count", "    .balign 8", f"simd_case_count: .quad {len(self.cases)}",
                "    .global simd_case_names", "simd_case_names:"]
        out += [f"    .quad sname_{i}" for i in range(len(self.cases))]
        out += [f"sname_{i}: .asciz \"{c[0]}\"" for i, c in enumerate(self.cases)]
        open(path, "w").write("\n".join(out) + "\n")


def main():
    path = sys.argv[1]
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 2024
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 12000
    g = Gen(seed)
    g.build(count)
    g.emit(path)
    print(f"{len(g.cases)} SIMD/FP cases")


if __name__ == "__main__":
    main()
