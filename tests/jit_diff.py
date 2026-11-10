#!/usr/bin/env python3
"""Find the first instruction where the block translator and the interpreter
disagree: runs a guest twice with -T (state checksum per instruction), the
second time with -J and one-instruction blocks, and reports the divergence.

  jit_diff.py VM ELF [extra args...]
"""
import os
import subprocess
import sys


def trace(vm, args, env):
    p = subprocess.run([vm, "-T", "-q", "-n", "50000000"] + args, capture_output=True, text=True, env=env)
    return [l for l in p.stderr.splitlines() if l and l[0].isdigit() and len(l.split()) == 3], p.stdout


def main():
    vm, elf, extra = sys.argv[1], sys.argv[2], sys.argv[3:]
    env = dict(os.environ, WD_JIT_MAX="1")
    a, _ = trace(vm, extra + [elf], env)
    b, out = trace(vm, ["-J"] + extra + [elf], env)
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            print(f"first difference at step {i}:")
            for k in range(max(0, i - 6), min(i + 2, len(a))):
                print(f"  interp {a[k]:45s}  jit {b[k] if k < len(b) else ''}")
            pc = int(a[i - 1].split()[1], 16) if i else 0
            dis = subprocess.run(["llvm-objdump", "-d", "--no-show-raw-insn", elf], capture_output=True, text=True).stdout
            for line in dis.splitlines():
                t = line.strip().split(":")[0]
                try:
                    if pc - 8 <= int(t, 16) <= pc + 8:
                        print("   ", line.strip())
                except ValueError:
                    pass
            return 1
    print(f"no difference in {min(len(a), len(b))} steps (lengths {len(a)} / {len(b)})")
    print(out[-500:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
