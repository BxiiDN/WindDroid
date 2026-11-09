"""Reference model of AArch64 floating point and Advanced SIMD semantics.

Floating point is computed exactly with fractions.Fraction and rounded once,
following the FPRound/FPProcessNaN/FPToFixed pseudocode of the ARM ARM.
This is deliberately independent of the engine (which uses the host FPU).
"""
from fractions import Fraction
import math

IOC, DZC, OFC, UFC, IXC, IDC = 1, 2, 4, 8, 16, 128
QC = 1 << 27
RN, RP, RM, RZ, RNA, ODD = 0, 1, 2, 3, 4, 5
FMT = {16: (5, 10, 15), 32: (8, 23, 127), 64: (11, 52, 1023)}


def mask(bits):
    return (1 << bits) - 1


def sx(v, bits):
    v &= mask(bits)
    return v - (1 << bits) if v >> (bits - 1) else v


class St:
    def __init__(self, fpcr):
        self.fpcr = fpcr
        self.fpsr = 0

    @property
    def dn(self):
        return (self.fpcr >> 25) & 1

    @property
    def fz(self):
        return (self.fpcr >> 24) & 1

    @property
    def rmode(self):
        return (self.fpcr >> 22) & 3


# ------------------------------------------------------------------ encoding helpers

def default_nan(n):
    E, F, _ = FMT[n]
    return (mask(E) << F) | (1 << (F - 1))


def inf(sign, n):
    E, F, _ = FMT[n]
    return (sign << (n - 1)) | (mask(E) << F)


def zero(sign, n):
    return sign << (n - 1)


def max_normal(sign, n):
    E, F, _ = FMT[n]
    return (sign << (n - 1)) | ((mask(E) - 1) << F) | mask(F)


def neg(a, n):
    return a ^ (1 << (n - 1))


def fabs(a, n):
    return a & ~(1 << (n - 1))


def unpack(a, n, st, honor_fz=True):
    """-> (cls, sign, value, bits_after_flush); cls in zero/num/inf/qnan/snan"""
    E, F, bias = FMT[n]
    a &= mask(n)
    sign = a >> (n - 1)
    e = (a >> F) & mask(E)
    f = a & mask(F)
    if e == 0:
        if f == 0:
            return "zero", sign, Fraction(0), a
        if honor_fz and n != 16 and st.fz:
            st.fpsr |= IDC
            return "zero", sign, Fraction(0), zero(sign, n)
        v = Fraction(f, 1 << (bias - 1 + F))
        return "num", sign, -v if sign else v, a
    if e == mask(E):
        if f == 0:
            return "inf", sign, None, a
        return ("qnan" if (f >> (F - 1)) & 1 else "snan"), sign, None, a
    v = Fraction(f | (1 << F)) * Fraction(2) ** (e - bias - F)
    return "num", sign, -v if sign else v, a


def process_nan(cls, bits, n, st):
    E, F, _ = FMT[n]
    r = bits
    if cls == "snan":
        st.fpsr |= IOC
        r |= 1 << (F - 1)
    if st.dn:
        r = default_nan(n)
    return r


def process_nans(ops, n, st):
    """ops: list of (cls, bits). Returns result or None."""
    for cls, b in ops:
        if cls == "snan":
            return process_nan(cls, b, n, st)
    for cls, b in ops:
        if cls == "qnan":
            return process_nan(cls, b, n, st)
    return None


def floor_log2(x):
    """floor(log2(x)) for positive Fraction"""
    e = x.numerator.bit_length() - x.denominator.bit_length()
    if Fraction(2) ** e > x:
        e -= 1
    elif Fraction(2) ** (e + 1) <= x:
        e += 1
    return e


def fp_round(value, n, rmode, st, fz=True):
    E, F, bias = FMT[n]
    if value == 0:
        return zero(0, n)
    sign = 1 if value < 0 else 0
    mag = -value if sign else value
    e = floor_log2(mag)
    emin = 1 - bias
    if fz and n != 16 and st.fz and e < emin:
        st.fpsr |= UFC
        return zero(sign, n)
    tiny = e < emin
    lsb = (emin - F) if tiny else (e - F)
    scaled = mag / Fraction(2) ** lsb
    im = math.floor(scaled)
    err = scaled - im
    half = Fraction(1, 2)
    inc = False
    if err:
        if rmode == RN:
            inc = err > half or (err == half and im & 1)
        elif rmode == RNA:
            inc = err >= half
        elif rmode == RP:
            inc = not sign
        elif rmode == RM:
            inc = bool(sign)
        elif rmode == ODD:
            im |= 1
    if inc:
        im += 1
    if im >> (F + 1):
        im >>= 1
        lsb += 1
    if im >> F:
        ef, fr = lsb + F + bias, im - (1 << F)
    else:
        ef, fr = 0, im
    if ef >= mask(E):
        st.fpsr |= OFC | IXC
        to_inf = {RN: True, RNA: True, RP: not sign, RM: bool(sign), RZ: False, ODD: False}[rmode]
        return inf(sign, n) if to_inf else max_normal(sign, n)
    if err:
        st.fpsr |= IXC
        if tiny:
            st.fpsr |= UFC
    return (sign << (n - 1)) | (ef << F) | fr


def zero_result_sign(st):
    return 1 if st.rmode == RM else 0


# ------------------------------------------------------------------ arithmetic

def add(a, b, n, st, subtract=False):
    c1, s1, v1, a = unpack(a, n, st)
    c2, s2, v2, b = unpack(b, n, st)
    r = process_nans([(c1, a), (c2, b)], n, st)
    if r is not None:
        return r
    if subtract:
        s2 ^= 1
        v2 = -v2 if v2 is not None else None
    if c1 == "inf" and c2 == "inf" and s1 != s2:
        st.fpsr |= IOC
        return default_nan(n)
    if c1 == "inf":
        return inf(s1, n)
    if c2 == "inf":
        return inf(s2, n)
    if c1 == "zero" and c2 == "zero" and s1 == s2:
        return zero(s1, n)
    v = v1 + v2
    if v == 0:
        return zero(zero_result_sign(st), n)
    return fp_round(v, n, st.rmode, st)


def mul(a, b, n, st, mulx=False):
    c1, s1, v1, a = unpack(a, n, st)
    c2, s2, v2, b = unpack(b, n, st)
    r = process_nans([(c1, a), (c2, b)], n, st)
    if r is not None:
        return r
    s = s1 ^ s2
    if (c1 == "inf" and c2 == "zero") or (c1 == "zero" and c2 == "inf"):
        if mulx:
            E, F, bias = FMT[n]
            return (s << (n - 1)) | ((bias + 1) << F)
        st.fpsr |= IOC
        return default_nan(n)
    if c1 == "inf" or c2 == "inf":
        return inf(s, n)
    if c1 == "zero" or c2 == "zero":
        return zero(s, n)
    return fp_round(v1 * v2, n, st.rmode, st)


def div(a, b, n, st):
    c1, s1, v1, a = unpack(a, n, st)
    c2, s2, v2, b = unpack(b, n, st)
    r = process_nans([(c1, a), (c2, b)], n, st)
    if r is not None:
        return r
    s = s1 ^ s2
    if (c1 == "inf" and c2 == "inf") or (c1 == "zero" and c2 == "zero"):
        st.fpsr |= IOC
        return default_nan(n)
    if c1 == "inf" or c2 == "zero":
        if c1 != "inf":
            st.fpsr |= DZC
        return inf(s, n)
    if c1 == "zero" or c2 == "inf":
        return zero(s, n)
    return fp_round(v1 / v2, n, st.rmode, st)


def muladd(A, a, b, n, st):
    cA, sA, vA, A = unpack(A, n, st)
    c1, s1, v1, a = unpack(a, n, st)
    c2, s2, v2, b = unpack(b, n, st)
    inf_zero = (c1 == "inf" and c2 == "zero") or (c1 == "zero" and c2 == "inf")
    if cA == "qnan" and inf_zero:
        st.fpsr |= IOC
        return default_nan(n)
    r = process_nans([(cA, A), (c1, a), (c2, b)], n, st)
    if r is not None:
        return r
    infP = c1 == "inf" or c2 == "inf"
    zeroP = c1 == "zero" or c2 == "zero"
    sP = s1 ^ s2
    if inf_zero or (cA == "inf" and infP and sA != sP):
        st.fpsr |= IOC
        return default_nan(n)
    if (cA == "inf" and sA == 0) or (infP and sP == 0):
        return inf(0, n)
    if (cA == "inf" and sA == 1) or (infP and sP == 1):
        return inf(1, n)
    if cA == "zero" and zeroP and sA == sP:
        return zero(sA, n)
    v = vA + v1 * v2
    if v == 0:
        return zero(zero_result_sign(st), n)
    return fp_round(v, n, st.rmode, st)


def sqrt_exact_round(v, n, st):
    # v > 0 Fraction. Compute sqrt with plenty of bits plus a sticky term.
    num, den = v.numerator, v.denominator
    # enough fraction bits that the integer square root has >= 120 significant bits
    k = max(200, (den.bit_length() - num.bit_length()) // 2 + 130)
    x = (num << (2 * k)) // den
    exact_div = (num << (2 * k)) % den == 0
    s = math.isqrt(x)
    approx = Fraction(s, 1 << k)
    if not (exact_div and s * s == x):
        approx += Fraction(1, 1 << (k + 20))
    return fp_round(approx, n, st.rmode, st)


def fsqrt(a, n, st):
    c, s, v, a = unpack(a, n, st)
    if c in ("qnan", "snan"):
        return process_nan(c, a, n, st)
    if c == "zero":
        return a
    if s:
        st.fpsr |= IOC
        return default_nan(n)
    if c == "inf":
        return a
    return sqrt_exact_round(v, n, st)


def maxmin(a, b, n, st, is_max, num):
    c1, s1, v1, a = unpack(a, n, st)
    c2, s2, v2, b = unpack(b, n, st)
    if num:
        if c1 == "qnan" and c2 not in ("qnan", "snan"):
            return b
        if c2 == "qnan" and c1 not in ("qnan", "snan"):
            return a
    r = process_nans([(c1, a), (c2, b)], n, st)
    if r is not None:
        return r
    if c1 == "zero" and c2 == "zero":
        return zero((s1 & s2) if is_max else (s1 | s2), n)

    def val(c, s, v):
        if c == "inf":
            return float("-inf") if s else float("inf")
        return v
    x, y = val(c1, s1, v1), val(c2, s2, v2)
    if is_max:
        return a if x > y else b
    return a if x < y else b


def compare(a, b, n, st, signal_all):
    c1, _, v1, _ = unpack(a, n, st)
    c2, _, v2, _ = unpack(b, n, st)
    if "nan" in c1 or "nan" in c2:
        if c1 == "snan" or c2 == "snan" or signal_all:
            st.fpsr |= IOC
        return None
    def val(c, a, v):
        if c == "inf":
            return float("-inf") if a >> (n - 1) & 1 else float("inf")
        return v
    x, y = val(c1, a, v1), val(c2, b, v2)
    return (x > y) - (x < y)


def fcmp_flags(a, b, n, st, signal_all):
    k = compare(a, b, n, st, signal_all)
    if k is None:
        return 0x3 << 28
    return {0: 0x6, -1: 0x8, 1: 0x2}[k] << 28


def cmp_eq(a, b, n, st):
    k = compare(a, b, n, st, False)
    return k == 0


def cmp_ge(a, b, n, st):
    k = compare(a, b, n, st, True)
    return k is not None and k >= 0


def cmp_gt(a, b, n, st):
    k = compare(a, b, n, st, True)
    return k is not None and k > 0


def round_to_int(v, rmode):
    """round Fraction to integer; returns (int, inexact)"""
    fl = math.floor(v)
    err = v - fl
    if err == 0:
        return fl, False
    half = Fraction(1, 2)
    if rmode == RN:
        up = err > half or (err == half and fl & 1)
    elif rmode == RNA:
        up = err > half or (err == half and v > 0)
    elif rmode == RP:
        up = True
    elif rmode == RM:
        up = False
    else:  # RZ
        up = v < 0
    return fl + (1 if up else 0), True


def round_int(a, n, rmode, exact, st):
    c, s, v, a = unpack(a, n, st)
    if "nan" in c:
        return process_nan(c, a, n, st)
    if c in ("inf", "zero"):
        return a
    r, inexact = round_to_int(v, rmode)
    if exact and inexact:
        st.fpsr |= IXC
    if r == 0:
        return zero(s, n)
    tmp = St(0)
    return fp_round(Fraction(r), n, RN, tmp)


def to_fixed(a, n, fbits, unsigned, rmode, outbits, st):
    c, s, v, a = unpack(a, n, st)
    if "nan" in c:
        st.fpsr |= IOC
        return 0
    if c == "inf":
        v = Fraction(-(1 << 2000)) if s else Fraction(1 << 2000)
    elif c == "zero":
        v = Fraction(0)
    v = v * Fraction(2) ** fbits
    r, inexact = round_to_int(v, rmode)
    if unsigned:
        lo, hi = 0, (1 << outbits) - 1
    else:
        lo, hi = -(1 << (outbits - 1)), (1 << (outbits - 1)) - 1
    if r < lo or r > hi:
        st.fpsr |= IOC
        r = lo if r < lo else hi
    elif inexact:
        st.fpsr |= IXC
    return r & mask(outbits)


def from_fixed(x, signed, inbits, fbits, n, rmode, st):
    x &= mask(inbits)
    if signed:
        x = sx(x, inbits)
    if x == 0:
        return zero(0, n)
    return fp_round(Fraction(x, 1 << fbits), n, rmode, st)


def convert(a, frm, to, rmode, st):
    c, s, v, a = unpack(a, frm, st)
    if "nan" in c:
        if c == "snan":
            st.fpsr |= IOC
        if st.dn:
            return default_nan(to)
        Ei, Fi, _ = FMT[frm]
        Eo, Fo, _ = FMT[to]
        payload = a & mask(Fi - 1)
        p51 = payload << (51 - (Fi - 1))
        po = p51 >> (51 - (Fo - 1))
        return (s << (to - 1)) | (mask(Eo) << Fo) | (1 << (Fo - 1)) | po
    if c == "inf":
        return inf(s, to)
    if c == "zero":
        return zero(s, to)
    return fp_round(v, to, rmode, st)


# ------------------------------------------------------------------ estimates

def recip_estimate(a):
    a = a * 2 + 1
    b = (1 << 19) // a
    return (b + 1) // 2


def rsqrt_estimate(a):
    if a < 256:
        a = a * 2 + 1
    else:
        a = (a >> 1) << 1
        a = (a + 1) * 2
    b = 512
    while a * (b + 1) * (b + 1) < (1 << 28):
        b += 1
    return (b + 1) // 2


def recpe(a, n, st):
    E, F, bias = FMT[n]
    c, s, v, a = unpack(a, n, st)
    if "nan" in c:
        return process_nan(c, a, n, st)
    if c == "inf":
        return zero(s, n)
    if c == "zero":
        st.fpsr |= DZC
        return inf(s, n)
    if abs(v) < Fraction(2) ** -(bias + 1):
        st.fpsr |= OFC | IXC
        to_inf = {RN: True, RP: not s, RM: bool(s), RZ: False}[st.rmode]
        return inf(s, n) if to_inf else max_normal(s, n)
    if st.fz and n != 16 and abs(v) >= Fraction(2) ** (bias - 1):
        st.fpsr |= UFC
        return zero(s, n)
    exp = (a >> F) & mask(E)
    fraction = (a & mask(F)) << (52 - F)
    if exp == 0:
        if not (fraction >> 51) & 1:
            exp = -1
            fraction = (fraction << 2) & mask(52)
        else:
            fraction = (fraction << 1) & mask(52)
    scaled = 256 | (fraction >> 44)
    rexp = 2 * bias - 1 - exp
    est = recip_estimate(scaled)
    fr = (est & 0xFF) << 44
    if rexp == 0:
        fr = (1 << 51) | (fr >> 1)
    elif rexp == -1:
        fr = (1 << 50) | (fr >> 2)
        rexp = 0
    return (s << (n - 1)) | (rexp << F) | (fr >> (52 - F))


def rsqrte(a, n, st):
    E, F, bias = FMT[n]
    c, s, v, a = unpack(a, n, st)
    if "nan" in c:
        return process_nan(c, a, n, st)
    if c == "zero":
        st.fpsr |= DZC
        return inf(s, n)
    if s:
        st.fpsr |= IOC
        return default_nan(n)
    if c == "inf":
        return zero(0, n)
    exp = (a >> F) & mask(E)
    fraction = (a & mask(F)) << (52 - F)
    if exp == 0:
        while not (fraction >> 51) & 1:
            fraction = (fraction << 1) & mask(52)
            exp -= 1
        fraction = (fraction << 1) & mask(52)
    if exp & 1:
        scaled = 128 | (fraction >> 45)
    else:
        scaled = 256 | (fraction >> 44)
    rexp = (3 * bias - 1 - exp) // 2
    est = rsqrt_estimate(scaled)
    return (rexp << F) | ((est & 0xFF) << (F - 8))


def urecpe(a):
    if not a >> 31:
        return 0xFFFFFFFF
    return recip_estimate(a >> 23) << 23


def ursqrte(a):
    if not a >> 30:
        return 0xFFFFFFFF
    return rsqrt_estimate(a >> 23) << 23


def recps(a, b, n, st):
    a = neg(a, n)
    c1, s1, v1, a = unpack(a, n, st)
    c2, s2, v2, b = unpack(b, n, st)
    r = process_nans([(c1, a), (c2, b)], n, st)
    if r is not None:
        return r
    E, F, bias = FMT[n]
    if (c1 == "inf" and c2 == "zero") or (c1 == "zero" and c2 == "inf"):
        return (bias + 1) << F
    if c1 == "inf" or c2 == "inf":
        return inf(s1 ^ s2, n)
    v = 2 + v1 * v2
    if v == 0:
        return zero(zero_result_sign(st), n)
    return fp_round(v, n, st.rmode, st)


def rsqrts(a, b, n, st):
    a = neg(a, n)
    c1, s1, v1, a = unpack(a, n, st)
    c2, s2, v2, b = unpack(b, n, st)
    r = process_nans([(c1, a), (c2, b)], n, st)
    if r is not None:
        return r
    E, F, bias = FMT[n]
    if (c1 == "inf" and c2 == "zero") or (c1 == "zero" and c2 == "inf"):
        return (bias << F) | (1 << (F - 1))
    if c1 == "inf" or c2 == "inf":
        return inf(s1 ^ s2, n)
    v = (3 + v1 * v2) / 2
    if v == 0:
        return zero(zero_result_sign(st), n)
    return fp_round(v, n, st.rmode, st)


def recpx(a, n, st):
    E, F, bias = FMT[n]
    c, s, v, a = unpack(a, n, st)
    if "nan" in c:
        return process_nan(c, a, n, st)
    exp = (a >> F) & mask(E)
    rexp = (mask(E) - 1) if exp == 0 else (~exp & mask(E))
    return (s << (n - 1)) | (rexp << F)


def fp_imm(imm8, n):
    E, F, _ = FMT[n]
    sign = (imm8 >> 7) & 1
    b6 = (imm8 >> 6) & 1
    exp = ((b6 ^ 1) << (E - 1)) | ((mask(E - 3) if b6 else 0) << 2) | ((imm8 >> 4) & 3)
    return (sign << (n - 1)) | (exp << F) | ((imm8 & 15) << (F - 4))
