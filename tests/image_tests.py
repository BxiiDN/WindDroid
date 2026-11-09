#!/usr/bin/env python3
"""
Disk image layer tests (raw, ISO, VHD, VHDX).

The VHD/VHDX samples in tests/samples come from QEMU's qemu-iotests
(GPL-2.0); they were created by the native Microsoft tools (Hyper-V,
Virtual PC, Disk2VHD), so reading them checks the parser against real
files rather than against our own writer. The expected contents are the
ones QEMU's iotests 064, 070 and 146 check.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import bz2
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "build", "disktool")
SAMPLES = os.path.join(ROOT, "tests", "samples")
M = 1 << 20

failures = []


def tool(*args):
    p = subprocess.run([TOOL] + [str(a) for a in args], capture_output=True)
    return p.returncode, (p.stdout + p.stderr).decode(errors="replace")


def check(name, cond, detail=""):
    print(("PASS  " if cond else "FAIL  ") + name + ("" if cond else "  " + detail))
    if not cond:
        failures.append(name)


def ok(name, *args):
    rc, out = tool(*args)
    check(name, rc == 0, out.strip())
    return out


def sample(d, name):
    path = os.path.join(d, name)
    with bz2.open(os.path.join(SAMPLES, name + ".bz2")) as src, open(path, "wb") as dst:
        shutil.copyfileobj(src, dst)
    return path


def crc32c(data):
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        table.append(c)
    crc = 0xFFFFFFFF
    for b in data:
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def guid(s):
    import uuid
    return uuid.UUID(s).bytes_le


def validate_vhdx(path, size, block, sector):
    """Structural check of a VHDX we created, following [MS-VHDX] section 2."""
    f = open(path, "rb").read(4 << 20)
    errs = []
    if f[:8] != b"vhdxfile":
        errs.append("file type identifier")
    for off in (64 << 10, 128 << 10):
        h = bytearray(f[off:off + 4096])
        crc = struct.unpack_from("<I", h, 4)[0]
        h[4:8] = b"\0\0\0\0"
        if h[:4] != b"head" or crc32c(bytes(h)) != crc:
            errs.append(f"header at {off:#x}")
        if struct.unpack_from("<H", h, 66)[0] != 1 or any(h[48:64]):
            errs.append("header version/log guid")
        log_len, log_off = struct.unpack_from("<IQ", h, 68)
        if log_len % M or log_off % M or log_off < M:
            errs.append("log placement")
    regions = {}
    for off in (192 << 10, 256 << 10):
        r = bytearray(f[off:off + (64 << 10)])
        crc = struct.unpack_from("<I", r, 4)[0]
        r[4:8] = b"\0\0\0\0"
        if r[:4] != b"regi" or crc32c(bytes(r)) != crc:
            errs.append(f"region table at {off:#x}")
        for i in range(struct.unpack_from("<I", r, 8)[0]):
            g, fo, ln, fl = struct.unpack_from("<16sQII", r, 16 + 32 * i)
            regions[g] = (fo, ln, fl)
            if fo % M or ln % M:
                errs.append("region alignment")
    bat = regions.get(guid("2DC27766-F623-4200-9D64-115E9BFD4A08"))
    meta = regions.get(guid("8B7CA206-4790-4B9A-B8FE-575F050F886E"))
    if not bat or not meta:
        errs.append("BAT/metadata regions")
        return errs
    mt = f[meta[0]:meta[0] + meta[1]]
    if mt[:8] != b"metadata":
        errs.append("metadata signature")
    items = {}
    for i in range(struct.unpack_from("<H", mt, 10)[0]):
        g, o, ln, fl = struct.unpack_from("<16sIII", mt, 32 + 32 * i)
        items[g] = (mt[o:o + ln], fl)
        if o < (64 << 10):
            errs.append("metadata item offset")
    def item(g):
        return items.get(guid(g), (b"", 0))
    bs, flags = struct.unpack("<II", item("CAA16737-FA36-4D43-B3B6-33F0AA44E76B")[0])
    vs = struct.unpack("<Q", item("2FA54224-CD1B-4876-B211-5DBED83BF4B8")[0])[0]
    lss = struct.unpack("<I", item("8141BF1D-A96F-4709-BA47-F233A8FAAB5F")[0])[0]
    pss = struct.unpack("<I", item("CDA348C7-445D-4471-9CC9-E9885251C556")[0])[0]
    if len(item("BECA12AB-B2E6-4523-93EF-C309E000C746")[0]) != 16:
        errs.append("virtual disk id")
    if (bs, vs, lss) != (block, size, sector) or pss not in (512, 4096) or flags:
        errs.append(f"metadata values {bs} {vs} {lss} {pss} {flags}")
    for g, (data, fl) in items.items():
        if not fl & 4:
            errs.append("metadata item not marked required")
    chunk = (1 << 23) * sector // block
    blocks = -(-size // block)
    entries = blocks + (blocks - 1) // chunk
    if bat[1] < entries * 8:
        errs.append("BAT too small")
    return errs


def main():
    if not os.path.exists(TOOL):
        print("build/disktool missing (run make)")
        return 1
    with tempfile.TemporaryDirectory() as d:
        # --- QEMU iotest 064: Hyper-V dynamic VHDX, 1 GiB
        img = sample(d, "iotest-dynamic-1G.vhdx")
        out = ok("vhdx_hyperv_info", "info", img)
        check("vhdx_hyperv_size", "format=vhdx size=1073741824 block=512" in out, out)
        ok("vhdx_hyperv_read_a5", "check", img, 0, "33M", 0xA5)
        ok("vhdx_hyperv_read_96", "check", img, "33M", "33M", 0x96)
        ok("vhdx_hyperv_read_zero", "check", img, "66M", "958M", 0)
        ok("vhdx_hyperv_write", "fill", img, "99M", "58M", 0xC3)
        ok("vhdx_hyperv_untouched_a5", "check", img, 0, "33M", 0xA5)
        ok("vhdx_hyperv_untouched_96", "check", img, "33M", "33M", 0x96)
        ok("vhdx_hyperv_untouched_0", "check", img, "66M", "33M", 0)
        ok("vhdx_hyperv_untouched_tail", "check", img, "157M", "867M", 0)
        ok("vhdx_hyperv_written", "check", img, "99M", "58M", 0xC3)
        ok("vhdx_hyperv_fuzz", "fuzz", img, 11, 3000)

        # --- QEMU iotest 070: dirty log written by Hyper-V
        img = sample(d, "iotest-dirtylog-10G-4M.vhdx")
        rc, out = tool("check", img, 0, "18M", 0xA5)
        check("vhdx_log_readonly_refused", rc != 0 and "log" in out, out)
        ok("vhdx_log_replay", "open-rw", img)
        ok("vhdx_log_data", "check", img, 0, "18M", 0xA5)
        ok("vhdx_log_rest_zero", "check", img, "18M", "1G", 0)
        out = ok("vhdx_log_info_after", "info", img)
        check("vhdx_log_size", "size=10737418240" in out, out)
        # the replay matters: with the log simply discarded the data is wrong
        raw = bytearray(open(sample(d, "iotest-dirtylog-10G-4M.vhdx"), "rb").read())
        for off in (64 << 10, 128 << 10):
            h = bytearray(raw[off:off + 4096])
            h[48:64] = bytes(16)
            h[4:8] = bytes(4)
            struct.pack_into("<I", h, 4, crc32c(bytes(h)))
            raw[off:off + 4096] = h
        nolog = os.path.join(d, "nolog.vhdx")
        open(nolog, "wb").write(raw)
        rc, out = tool("check", nolog, 0, "18M", 0xA5)
        check("vhdx_log_needed", rc == 1 and "mismatch" in out, out)

        # --- Disk2VHD image
        img = sample(d, "test-disk2vhd.vhdx")
        out = ok("vhdx_disk2vhd_info", "info", img)
        check("vhdx_disk2vhd_size", "size=268435456" in out, out)

        # --- QEMU iotest 146: VHD files (sizes from the footer's current size, like Hyper-V)
        for name, size in (("virtualpc-dynamic.vhd", 136365211648), ("hyperv2012r2-dynamic.vhd", 136365211648),
                           ("d2v-zerofilled.vhd", 263454720)):
            img = sample(d, name)
            out = ok("vhd_info_" + name, "info", img)
            check("vhd_size_" + name, f"format=vhd size={size}" in out, out)
            ok("vhd_zero_" + name, "check", img, 0, "64M", 0)
            ok("vhd_fuzz_" + name, "fuzz", img, 7, 3000)
        img = sample(d, "hyperv2012r2-dynamic.vhd")
        ok("vhd_write_far", "fill", img, "100G", "1M", 0x33)
        ok("vhd_write_near", "fill", img, 1000000, 3000000, 0x5A)
        ok("vhd_write_inner", "fill", img, 1001000, 512, 0x77)
        ok("vhd_read_before", "check", img, 0, 1000000, 0)
        ok("vhd_read_a", "check", img, 1000000, 1000, 0x5A)
        ok("vhd_read_inner", "check", img, 1001000, 512, 0x77)
        ok("vhd_read_b", "check", img, 1001512, 2998488, 0x5A)
        ok("vhd_read_after", "check", img, 4000000, "100M", 0)
        ok("vhd_read_far", "check", img, "100G", "1M", 0x33)

        # --- VHDX we create
        for size, block, sector in ((200 * M, 1 * M, 512), (200 * M, 2 * M, 4096), (64 << 30, 32 * M, 512)):
            img = os.path.join(d, "new.vhdx")
            ok(f"vhdx_create_{size >> 20}M_{block >> 20}M_{sector}", "create", img, size, block, sector)
            errs = validate_vhdx(img, size, block, sector)
            check(f"vhdx_structure_{size >> 20}M_{block >> 20}M_{sector}", not errs, ", ".join(errs))
            out = ok("vhdx_new_info", "info", img)
            check("vhdx_new_params", f"size={size} block={sector}" in out, out)
            check("vhdx_new_sparse", os.path.getsize(img) < 8 * M + size // block * 8, str(os.path.getsize(img)))
            ok("vhdx_new_fuzz", "fuzz", img, 3, 4000)
            errs = validate_vhdx(img, size, block, sector)
            check("vhdx_structure_after_writes", not errs, ", ".join(errs))

        # --- raw and ISO detection
        raw = os.path.join(d, "disk.img")
        with open(raw, "wb") as f:
            f.truncate(32 * M)
        out = ok("raw_info", "info", raw)
        check("raw_params", "format=raw size=33554432 block=512" in out, out)
        ok("raw_fuzz", "fuzz", raw, 5, 2000)
        iso = os.path.join(d, "cd.iso")
        with open(iso, "wb") as f:
            f.truncate(4 * M)
            f.seek(0x8000)
            f.write(b"\x01CD001\x01")
        out = ok("iso_info", "info", iso)
        check("iso_params", "format=iso size=4194304 block=2048" in out, out)

        # --- convert raw -> VHDX keeps the contents
        src = os.path.join(d, "src.img")
        blob = os.urandom(3 * M + 4096)
        with open(src, "wb") as f:
            f.truncate(48 * M)
            f.seek(5 * M + 512)
            f.write(blob)
        conv = os.path.join(d, "conv.vhdx")
        ok("convert_raw_to_vhdx", "convert", src, conv)
        p = subprocess.run([TOOL, "dump", conv], capture_output=True)
        check("convert_contents", p.returncode == 0 and p.stdout == open(src, "rb").read(), "dump differs")
        # one 32 MiB block allocated after the 4 MiB of metadata; on disk only the written data uses space
        check("convert_one_block", os.path.getsize(conv) == 36 * M, str(os.path.getsize(conv)))
        check("convert_sparse", os.stat(conv).st_blocks * 512 < 12 * M, str(os.stat(conv).st_blocks * 512))

    print(f"image tests: {len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
