#!/usr/bin/env python3
"""
Build an ISO 9660 image with an El Torito boot catalog laid out like
Microsoft's Windows installation media: a validation entry for x86 with a
default (BIOS) entry, then a section for the EFI platform (0xEF) whose
no-emulation entry points at a FAT image containing \\EFI\\BOOT\\BOOTAA64.EFI.
Written from ECMA-119 and the El Torito 1.0 specification.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import struct

BLOCK = 2048


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def dir_record(name, lba, size, is_dir):
    date = bytes([126, 1, 1, 0, 0, 0, 0])            # 2026-01-01 00:00:00 UTC
    rec = struct.pack("<BB", 0, 0) + both32(lba) + both32(size) + date
    rec += bytes([2 if is_dir else 0, 0, 0]) + both16(1) + bytes([len(name)]) + name
    if len(rec) % 2:
        rec += b"\0"
    return bytes([len(rec)]) + rec[1:]


def pad(data):
    return data + bytes(-len(data) % BLOCK)


def build(path, efi_image, files=(), volume_id="WINDDROID_TEST"):
    files = [(n.upper() + ";1", d) for n, d in files]
    bios_lba = 23
    efi_lba = bios_lba + 1
    efi_blocks = -(-len(efi_image) // BLOCK)
    next_lba = efi_lba + efi_blocks
    placed = []
    for name, data in files:
        placed.append((name, next_lba, data))
        next_lba += max(1, -(-len(data) // BLOCK))
    total = next_lba

    root = dir_record(b"\0", 22, BLOCK, True) + dir_record(b"\1", 22, BLOCK, True)
    for name, lba, data in placed:
        root += dir_record(name.encode(), lba, len(data), False)
    root += dir_record(b"EFISYS.BIN;1", efi_lba, len(efi_image), False)
    assert len(root) <= BLOCK

    pvd = bytearray(BLOCK)
    pvd[0:7] = b"\x01CD001\x01"
    pvd[8:40] = b" " * 32
    pvd[40:72] = volume_id.encode().ljust(32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(BLOCK)
    pvd[132:140] = both32(10)
    pvd[140:144] = struct.pack("<I", 20)
    pvd[148:152] = struct.pack(">I", 21)
    pvd[156:190] = dir_record(b"\0", 22, BLOCK, True)
    pvd[190:813] = b" " * 623
    for off in (813, 830, 847, 864):
        pvd[off:off + 17] = b"2026010100000000\0" if off != 847 else b"0000000000000000\0"
    pvd[881] = 1

    brvd = bytearray(BLOCK)
    brvd[0:7] = b"\x00CD001\x01"
    brvd[7:39] = b"EL TORITO SPECIFICATION".ljust(32, b"\0")
    brvd[71:75] = struct.pack("<I", 19)

    term = bytearray(BLOCK)
    term[0:7] = b"\xffCD001\x01"

    cat = bytearray(BLOCK)
    val = bytearray(32)
    val[0] = 1
    val[1] = 0                                        # x86, like Windows media
    val[4:28] = b"WindDroid".ljust(24, b"\0")
    val[30:32] = b"\x55\xAA"
    csum = sum(struct.unpack("<16H", bytes(val))) & 0xFFFF
    struct.pack_into("<H", val, 28, (-csum) & 0xFFFF)
    assert sum(struct.unpack("<16H", bytes(val))) & 0xFFFF == 0
    cat[0:32] = val
    cat[32:64] = struct.pack("<BBHBBHI", 0x88, 0, 0, 0, 0, 4, bios_lba) + bytes(20)   # BIOS entry
    cat[64:96] = struct.pack("<BBH", 0x91, 0xEF, 1) + b"UEFI".ljust(28, b"\0")       # final section: EFI
    sectors = len(efi_image) // 512
    assert sectors <= 0xFFFF
    cat[96:128] = struct.pack("<BBHBBHI", 0x88, 0, 0, 0, 0, sectors, efi_lba) + bytes(20)

    pt_root = struct.pack("<BBIH", 1, 0, 22, 1) + b"\0\0"
    lpt = pad(pt_root)
    mpt = pad(struct.pack(">BBIH", 1, 0, 22, 1) + b"\0\0")

    with open(path, "wb") as f:
        f.write(bytes(16 * BLOCK))
        f.write(pvd + brvd + term + cat + lpt + mpt + pad(root))
        f.write(pad(b"\xF4" * 16))                    # placeholder BIOS boot image (never run here)
        f.write(pad(efi_image))
        for name, lba, data in placed:
            assert f.tell() == lba * BLOCK
            f.write(pad(data) if data else bytes(BLOCK))
        assert f.tell() == total * BLOCK
