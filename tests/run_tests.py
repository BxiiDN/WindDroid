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
    ("uefi_gop", ["-s", "Please select boot device", "-k", "Press any key=\\r", "-F", "{screen}"],
     ["Please select boot device"]),
]


def check_screen(path):
    """The boot manager dialog: blue box with white text on an 800x600 GOP screen."""
    try:
        d = open(path, "rb").read()
    except OSError:
        return "no framebuffer image written"
    hdr = d.split(b"\n", 3)
    if hdr[0] != b"P6" or hdr[1] != b"800 600":
        return f"unexpected framebuffer {hdr[:2]}"
    px = hdr[3]
    blue = white = 0
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if r < 40 and g < 40 and b > 120:
            blue += 1
        elif r > 180 and g > 180 and b > 180:
            white += 1
    if blue < 20000 or white < 1000:
        return f"screen content unexpected (blue={blue}, white={white})"
    return None


def run_fw_tests(vm, quiet, timeout):
    import shutil
    import tempfile
    failed = []
    with tempfile.TemporaryDirectory() as d:
        vars_path = os.path.join(d, "vars.fd")
        shutil.copy(os.path.join(FW, "QEMU_VARS.fd"), vars_path)
        template = open(vars_path, "rb").read()
        for name, extra, want in FW_TESTS:
            screen = os.path.join(d, "screen.ppm")
            cmd = [vm, "-q", "-r", "-m", "1024", "-n", "60000000000", "-b", os.path.join(FW, "QEMU_EFI.fd"),
                   "-v", vars_path] + [a.replace("{screen}", screen) for a in extra]
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
            if name == "uefi_gop":
                err = check_screen(screen)
                if err:
                    extra_fail.append(err)
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


# ------------------------------------------------------------------ disks (NVMe)

SHELL_SCRIPT = (
    "echo -off\r\n"
    "ls fs0:\\\r\n"
    "type fs0:\\hello.txt\r\n"
    "cp fs0:\\big.bin fs0:\\copy.bin\r\n"
    "echo written > fs0:\\out.txt\r\n"
    "if exist fs1:\\second.txt then\r\n  type fs1:\\second.txt\r\nendif\r\n"
    "pci\r\n"
    "echo SCRIPT-DONE\r\n"
    "reset -s\r\n"
)


RO_SCRIPT = (
    "echo -off\r\n"
    "type fs0:\\hello.txt\r\n"
    "cp fs0:\\hello.txt fs0:\\copy.txt\r\n"
    "echo SCRIPT-DONE\r\n"
    "reset -s\r\n"
)


def make_shell_disk(path, size_mb=64, extra=(), script=SHELL_SCRIPT):
    sys.path.insert(0, os.path.join(ROOT, "tests"))
    import fatdisk
    big = bytes((i * 131 + (i >> 12)) & 0xFF for i in range(3000000))
    files = [("EFI/BOOT/BOOTAA64.EFI", open(os.path.join(FW, "Shell.efi"), "rb").read()),
             ("startup.nsh", script.encode()),
             ("hello.txt", b"Hello from the NVMe disk\r\n"),
             ("big.bin", big)] + list(extra)
    fatdisk.create(path, size_mb, files)
    return big


def esp_file(raw_path, name):
    sys.path.insert(0, os.path.join(ROOT, "tests"))
    import fatdisk
    try:
        return fatdisk.Reader(raw_path).cat(name)
    except (FileNotFoundError, AssertionError):
        return None


def run_disk_tests(vm, quiet, timeout):
    import shutil
    import tempfile
    tool = os.path.join(ROOT, "build", "disktool")
    failed = []
    with tempfile.TemporaryDirectory() as d:
        def boot(name, disks, want, verify):
            vars_path = os.path.join(d, "vars.fd")
            shutil.copy(os.path.join(FW, "QEMU_VARS.fd"), vars_path)
            cmd = [vm, "-q", "-m", "1024", "-n", "60000000000", "-b", os.path.join(FW, "QEMU_EFI.fd"),
                   "-v", vars_path]
            for disk in disks:
                cmd += ["-D", disk]
            t0 = time.time()
            try:
                p = subprocess.run(cmd, capture_output=True, timeout=timeout)
                out, code = (p.stdout + p.stderr).decode(errors="replace"), p.returncode
            except subprocess.TimeoutExpired as e:
                out, code = (e.stdout or b"").decode(errors="replace"), -1
            clean = re.sub(r"\x1b\[[0-9;=]*[A-Za-z]", " ", out)
            missing = [w for w in want if w not in clean]
            problems = verify() if code == 0 else []
            ok = code == 0 and not missing and not problems
            if not quiet or not ok:
                print(f"{'PASS' if ok else 'FAIL'}  {name:12s} exit={code:<4d} {time.time() - t0:5.2f}s")
            if not ok:
                failed.append(name)
                for w in missing:
                    print(f"      missing output: {w!r}")
                for w in problems:
                    print(f"      {w}")
                print("      " + "\n      ".join(clean.strip().splitlines()[-10:]))

        common = ["UEFI WindDroid NVMe Disk", "UEFI Interactive Shell", "BIG.BIN", "Hello from the NVMe disk",
                  "SCRIPT-DONE", "Non-volatile memory subsystem"]

        def verify_written(raw):
            def f():
                errs = []
                if esp_file(raw, "copy.bin") != big:
                    errs.append("copy.bin written by the guest differs from big.bin")
                if esp_file(raw, "out.txt") != "written\r\n".encode("utf-16"):
                    errs.append(f"out.txt content {esp_file(raw, 'out.txt')!r}")
                return errs
            return f

        # raw image, plus a second NVMe disk with its own FAT volume
        raw = os.path.join(d, "disk.img")
        big = make_shell_disk(raw)
        second = os.path.join(d, "second.img")
        sys.path.insert(0, os.path.join(ROOT, "tests"))
        import fatdisk
        fatdisk.create(second, 40, [("second.txt", b"second NVMe disk\r\n")])
        boot("nvme_raw", [raw, second], common + ["second NVMe disk"], verify_written(raw))

        # the same disk as a VHDX (blocks allocated by the guest's writes)
        raw2 = os.path.join(d, "disk2.img")
        make_shell_disk(raw2)
        vhdx = os.path.join(d, "disk.vhdx")
        subprocess.run([tool, "convert", raw2, vhdx], check=True)

        def verify_vhdx():
            back = os.path.join(d, "back.img")
            with open(back, "wb") as fo:
                subprocess.run([tool, "dump", vhdx], stdout=fo, check=True)
            return verify_written(back)()
        boot("nvme_vhdx", [vhdx], common, verify_vhdx)

        # read-only: the guest cannot change the image
        raw3 = os.path.join(d, "disk3.img")
        make_shell_disk(raw3, script=RO_SCRIPT)
        before = open(raw3, "rb").read()

        def verify_ro():
            return [] if open(raw3, "rb").read() == before else ["read-only image was modified"]
        boot("nvme_readonly", ["ro:" + raw3], ["UEFI Interactive Shell", "Hello from the NVMe disk",
                                               "failed to open with create", "SCRIPT-DONE"],
             verify_ro)
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
        failed += run_disk_tests(args.vm, args.quiet, max(args.timeout, 300))
        total += 3
        acpiexec = os.environ.get("ACPIEXEC") or os.path.join(ROOT, "..", "ext", "acpica", "generate", "unix", "bin", "acpiexec")
        if os.path.exists(acpiexec):
            total += 1
            p = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "acpi_check.py"), args.vm,
                                os.path.join(FW, "QEMU_EFI.fd"), acpiexec], capture_output=True, text=True)
            ok = p.returncode == 0
            if not args.quiet or not ok:
                print(f"{'PASS' if ok else 'FAIL'}  acpi_tables")
            if not ok:
                failed.append("acpi_tables")
                print("      " + "\n      ".join(p.stdout.strip().splitlines()[-15:]))
        else:
            print("SKIP  acpi_tables (set ACPIEXEC to ACPICA's acpiexec)")
    print(f"{total - len(failed)}/{total} tests passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
