#!/usr/bin/env python3
"""Run every guest test program on build/winddroid-vm and check results.

Usage: run_tests.py [--vm PATH] [--guests DIR] [--quiet]
Exit status 0 only if every test passes.
"""
import argparse
import re
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# name, extra vm args, substrings that must appear in the UART output
TESTS = [
    ("alu_gen", [], ["alu: 4000 checks, 0 failures"]),
    ("mem_gen", [], ["0 failures"]),
    ("t_sys", [], ["t_sys: 0 failures"]),
    ("t_mmu", [], ["t_mmu: 0 failures"]),
    ("t_el0", [], ["t_el0: 0 failures"]),
    ("t_irq", ["-i", "hey!"], ["ticks=5 sgis=2 uart=1 rx=\"hey!\" failures=0"]),
    ("t_cprog_O0", [], ["t_cprog: 0 failures"]),
    ("t_cprog_O2", [], ["t_cprog: 0 failures"]),
    ("t_cprog_Os", [], ["t_cprog: 0 failures"]),
    ("t_cprog_gro", [], ["t_cprog: 0 failures"]),
    ("simd_gen", [], ["simd: 12000 cases, 0 failures"]),
    ("t_simdmem", [], ["t_simdmem: 0 failures"]),
    ("t_float", [], subprocess.run([sys.executable, os.path.join(ROOT, "tests", "float_ref.py")],
                                   capture_output=True, text=True).stdout.split("\n")[:3]),
    ("monitor", ["-i", "help\\ninfo\\nselftest\\nprimes 50\\nmandel\\nbench\\npoweroff\\n"],
     ["WindDroid Monitor", "selftest: all 6 checks passed",
      "2 3 5 7 11 13 17 19 23 29 31 37 41 43 47", "sha256(abc) = ba7816bf", "Powering off"]),
]


FW = os.path.join(ROOT, "firmware")

# UEFI tests: (name, args, expected output, check) - args may use {vars}
FW_TESTS = [
    ("uefi_boot", ["-s", "Please select boot device", "-k", "Press any key=\\r"],
     ["UEFI firmware", "No bootable option or device was found", "Please select boot device"]),
    ("uefi_setup", ["-s", "Device Manager", "-k", "Press any key=\\r",
                    "-k", "ESC to exit=\\r"],
     ["Please select boot device", "Device Manager"]),
    ("uefi_vars_reload", ["-s", "Press any key"], ["Press any key to enter the Boot Manager Menu"]),
]


def run_fw_tests(vm, quiet, timeout):
    import shutil
    import tempfile
    failed = []
    with tempfile.TemporaryDirectory() as d:
        vars_path = os.path.join(d, "vars.fd")
        shutil.copy(os.path.join(FW, "QEMU_VARS.fd"), vars_path)
        template = open(vars_path, "rb").read()
        for name, extra, want in FW_TESTS:
            cmd = [vm, "-q", "-r", "-m", "1024", "-n", "60000000000", "-b", os.path.join(FW, "QEMU_EFI.fd"),
                   "-v", vars_path] + extra
            t0 = time.time()
            try:
                p = subprocess.run(cmd, capture_output=True, timeout=timeout)
                out, code = (p.stdout + p.stderr).decode(errors="replace"), p.returncode
            except subprocess.TimeoutExpired as e:
                out, code = (e.stdout or b"").decode(errors="replace"), -1
            clean = re.sub(r"\x1b\[[0-9;=]*[A-Za-z]", " ", out)
            missing = [w for w in want if w not in clean]
            extra_fail = []
            if name == "uefi_boot" and open(vars_path, "rb").read() == template:
                extra_fail.append("variable store was not written back")
            ok = code == 0 and not missing and not extra_fail
            if not quiet or not ok:
                print(f"{'PASS' if ok else 'FAIL'}  {name:12s} exit={code:<4d} {time.time() - t0:5.2f}s")
            if not ok:
                failed.append(name)
                for w in missing:
                    print(f"      missing output: {w!r}")
                for w in extra_fail:
                    print(f"      {w}")
                print("      " + "\n      ".join(clean.strip().splitlines()[-8:]))
    return failed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vm", default=os.path.join(ROOT, "build", "winddroid-vm"))
    ap.add_argument("--guests", default=os.path.join(ROOT, "guest", "build"))
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--timeout", type=float, default=120)
    ap.add_argument("--no-firmware", action="store_true")
    args = ap.parse_args()

    failed = []
    for name, extra, want in TESTS:
        elf = os.path.join(args.guests, name + ".elf")
        cmd = [args.vm, "-q", "-n", "400000000"] + extra + [elf]
        t0 = time.time()
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
            out, code = p.stdout + p.stderr, p.returncode
        except subprocess.TimeoutExpired as e:
            out, code = (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or ""), -1
        dt = time.time() - t0
        missing = [w for w in want if w not in out]
        ok = code == 0 and not missing
        if not args.quiet or not ok:
            print(f"{'PASS' if ok else 'FAIL'}  {name:12s} exit={code:<4d} {dt:5.2f}s")
        if not ok:
            failed.append(name)
            if not args.quiet:
                for w in missing:
                    print(f"      missing output: {w!r}")
                print("      " + "\n      ".join(out.strip().splitlines()[-12:]))
    total = len(TESTS)
    if not args.no_firmware and os.path.exists(os.path.join(FW, "QEMU_EFI.fd")):
        failed += run_fw_tests(args.vm, args.quiet, max(args.timeout, 300))
        total += len(FW_TESTS)
    print(f"{total - len(failed)}/{total} tests passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
