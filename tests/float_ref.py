#!/usr/bin/env python3
"""Recomputes guest/tests/t_float.c in Python (IEEE double; float32 by rounding each step)."""
import struct


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def fb(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def db(x):
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def h64(h, v):
    return ((h ^ (v & 0xFFFFFFFFFFFFFFFF)) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF


def main():
    N = 24
    fa = [[f32(f32(f32(i * 3 + j) / f32(7.0)) - f32(1.5)) for j in range(N)] for i in range(N)]
    fbm = [[f32(f32(f32(j * 5 - i) * f32(0.125)) + f32(0.3)) for j in range(N)] for i in range(N)]
    da = [[(i * 3 + j) / 7.0 - 1.5 for j in range(N)] for i in range(N)]
    dbm = [[(j * 5 - i) * 0.125 + 0.3 for j in range(N)] for i in range(N)]
    h = 0xCBF29CE484222325
    for i in range(N):
        for j in range(N):
            s, t = f32(0.0), 0.0
            for k in range(N):
                s = f32(s + f32(fa[i][k] * fbm[k][j]))
                t = t + da[i][k] * dbm[k][j]
            h = h64(h, fb(s))
            h = h64(h, db(t))
    out = [f"matmul {h:016x}"]
    v = [f32(f32(f32(i) * f32(0.37)) - f32(40.0)) for i in range(256)]
    w = [f32(f32(1.0) / f32(f32(i) + f32(0.5))) for i in range(256)]
    v = [f32(f32(v[i] * w[i]) + (v[i] if v[i] > 0 else -v[i])) for i in range(256)]
    h = 0xCBF29CE484222325
    for x in v:
        h = h64(h, fb(x))
    out.append(f"vec {h:016x}")
    h = 0xCBF29CE484222325
    for i in range(1, 200):
        x = i * 1.75
        r = x
        for _ in range(8):
            r = 0.5 * (r + x / r)
        p = ((0.25 * x - 1.5) * x + 3.0) * x - 7.0
        ip = int(p)
        sf = f32(r)
        h = h64(h, db(r))
        h = h64(h, db(p))
        h = h64(h, ip & 0xFFFFFFFFFFFFFFFF)
        h = h64(h, fb(sf))
        h = h64(h, int(f32(sf * f32(3.9))) & 0xFFFFFFFFFFFFFFFF)
    out.append(f"newton {h:016x}")
    print("\n".join(out))


if __name__ == "__main__":
    main()
