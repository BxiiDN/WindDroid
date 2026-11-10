#!/usr/bin/env python3
"""Validate WindDroid's ACPI tables.

1. Runs the fw_cfg table-loader script independently (allocate, add pointer,
   add checksum) like EDK2's AcpiPlatformDxe does.
2. Checks every table's length, checksum and the RSDP/XSDT/FADT links.
3. Loads the tables into ACPICA's acpiexec and evaluates the PCI root bridge
   methods (_CRS, _PRT, _OSC) and the UART resources.

Usage: acpi_check.py VM FIRMWARE ACPIEXEC
"""
import os
import struct
import subprocess
import sys
import tempfile


def run_loader(files, script):
    base = {"etc/acpi/rsdp": 0x1000, "etc/acpi/tables": 0x100000}
    blobs = {k: bytearray(v) for k, v in files.items()}
    for i in range(0, len(script), 128):
        e = script[i:i + 128]
        cmd = struct.unpack_from("<I", e)[0]
        name = e[4:60].split(b"\0")[0].decode()
        if cmd == 1:
            assert name in blobs, name
        elif cmd == 2:
            src = e[60:116].split(b"\0")[0].decode()
            off, size = struct.unpack_from("<IB", e, 116)
            v = int.from_bytes(blobs[name][off:off + size], "little") + base[src]
            blobs[name][off:off + size] = v.to_bytes(size, "little")
        elif cmd == 3:
            off, start, length = struct.unpack_from("<III", e, 60)
            blobs[name][off] = 0
            blobs[name][off] = (-sum(blobs[name][start:start + length])) & 0xFF
        elif cmd != 0:
            raise SystemExit(f"unknown loader command {cmd}")
    return blobs, base


def main():
    vm, fw, acpiexec = sys.argv[1:4]
    errors = []
    with tempfile.TemporaryDirectory() as d:
        subprocess.run([vm, "-q", "-m", "1024", "-b", fw, "-A", d], check=True)
        files = {"etc/acpi/tables": open(os.path.join(d, "tables.bin"), "rb").read(),
                 "etc/acpi/rsdp": open(os.path.join(d, "rsdp.bin"), "rb").read()}
        script = open(os.path.join(d, "loader.bin"), "rb").read()
        blobs, base = run_loader(files, script)
        rsdp = blobs["etc/acpi/rsdp"]
        tables = blobs["etc/acpi/tables"]
        if rsdp[:8] != b"RSD PTR " or sum(rsdp[:20]) & 0xFF or sum(rsdp[:36]) & 0xFF:
            errors.append("RSDP signature or checksum wrong")
        xsdt_addr = struct.unpack_from("<Q", rsdp, 24)[0]

        def table_at(addr):
            off = addr - base["etc/acpi/tables"]
            sig = tables[off:off + 4].decode()
            ln = struct.unpack_from("<I", tables, off + 4)[0]
            t = bytes(tables[off:off + ln])
            if sum(t) & 0xFF:
                errors.append(f"{sig}: bad checksum")
            return sig, t

        sig, xsdt = table_at(xsdt_addr)
        if sig != "XSDT":
            errors.append("RSDP does not point to the XSDT")
        found = {}
        for i in range((len(xsdt) - 36) // 8):
            s, t = table_at(struct.unpack_from("<Q", xsdt, 36 + 8 * i)[0])
            found[s] = t
        fadt = found.get("FACP")
        if not fadt or len(fadt) != 276:
            errors.append("FADT missing or wrong size")
        else:
            if not struct.unpack_from("<I", fadt, 112)[0] & (1 << 20):
                errors.append("FADT is not hardware reduced")
            if struct.unpack_from("<H", fadt, 129)[0] != 3:
                errors.append("FADT ARM boot flags must say PSCI over HVC")
            s, dsdt = table_at(struct.unpack_from("<Q", fadt, 140)[0])
            if s != "DSDT":
                errors.append("FADT.X_DSDT does not point to the DSDT")
            found["DSDT"] = dsdt
        for want in ("FACP", "APIC", "GTDT", "MCFG", "SPCR", "DBG2", "DSDT", "IORT"):
            if want not in found:
                errors.append(f"{want} missing")
        madt = found.get("APIC", b"")
        p, kinds = 44, []
        while p < len(madt):
            kinds.append((madt[p], madt[p + 1]))
            p += madt[p + 1]
        if p != len(madt) or (0xB, 80) not in kinds or (0xC, 24) not in kinds or (0xE, 16) not in kinds \
                or (0xF, 20) not in kinds:
            errors.append(f"MADT structures wrong: {kinds}")
        p = 44
        while p < len(madt):
            if madt[p] == 0xF and struct.unpack_from("<IQ", madt, p + 4) != (0, 0x08080000):
                errors.append("MADT GIC ITS entry wrong")
            p += madt[p + 1]
        # IORT (ARM DEN 0049): ITS group + root complex mapping all requester IDs to the ITS
        iort = found.get("IORT", b"")
        if len(iort) >= 48:
            count, first = struct.unpack_from("<II", iort, 36)
            nodes, q = {}, first
            for _ in range(count):
                typ, ln, rev = struct.unpack_from("<BHB", iort, q)
                nodes[q] = (typ, iort[q:q + ln])
                q += ln
            if q != len(iort):
                errors.append("IORT node lengths do not add up")
            its = [o for o, (t, _) in nodes.items() if t == 0]
            rc = [b for t, b in nodes.values() if t == 2]
            if len(its) != 1 or struct.unpack_from("<II", nodes[its[0]][1], 16) != (1, 0):
                errors.append("IORT ITS group node wrong")
            if len(rc) != 1:
                errors.append("IORT root complex node missing")
            else:
                nmap, ref = struct.unpack_from("<II", rc[0], 8)
                cca, hints, _, flags = struct.unpack_from("<IBHB", rc[0], 16)
                seg = struct.unpack_from("<I", rc[0], 28)[0]
                ib, num, ob, oref, mflags = struct.unpack_from("<IIIII", rc[0], ref)
                if nmap != 1 or cca != 1 or flags != 3 or seg != 0 or (ib, num, ob, mflags) != (0, 0xFFFF, 0, 0) \
                        or oref != its[0]:
                    errors.append("IORT root complex mapping wrong")
        else:
            errors.append("IORT too short")
        mcfg = found.get("MCFG", b"")
        if len(mcfg) >= 60:
            base_ecam, seg, b0, b1 = struct.unpack_from("<QHBB", mcfg, 44)
            if base_ecam != 0x3F000000 or b0 != 0 or b1 != 15:
                errors.append("MCFG entry wrong")
        # write files for acpiexec
        paths = []
        for s, t in found.items():
            if s in ("XSDT",):
                continue
            path = os.path.join(d, s.lower() + ".dat")
            open(path, "wb").write(t)
            paths.append(path)
        cmds = ["resources \\_SB.PCI0", "resources \\_SB.COM0", "resources \\_SB.PCI0.RES0",
                "evaluate \\_SB.PCI0._HID", "evaluate \\_SB.C000._HID", "evaluate \\_SB.COM0._HID",
                "evaluate \\_SB.PCI0._CCA", "namespace"]
        dsdt_path = os.path.join(d, "dsdt.dat")
        out = ""
        for c in cmds:
            p = subprocess.run([acpiexec, "-b", c, dsdt_path], capture_output=True, text=True)
            out += p.stdout + p.stderr
        bad = [l for l in out.splitlines()
               if (("AE_" in l and "AE_OK" not in l) or "Error" in l or "Warning" in l)
               and "Outstanding cache allocations" not in l and "AcpiSetCurrentResources" not in l]
        for l in bad[:20]:
            errors.append("acpiexec: " + l.strip())
        flat = " ".join(out.split())
        must = [
            "Address Minimum : 0000 Address Maximum : 000F",                 # buses 0-15
            "Address Minimum : 10000000 Address Maximum : 3EFEFFFF",         # 32-bit MMIO
            "Translation Offset : 3EFF0000 Address Length : 00010000",       # I/O window
            "Address Minimum : 0000008000000000",                            # 64-bit MMIO
            "Address Minimum : 000000003F000000",                            # ECAM reservation
            "Address : 09000000",                                            # UART registers
            "Source Index : 00000023",                                       # slot 0 INTA -> GSI 35
            "ARMH0011", "ACPI0007", "080AD041", "_OSC",               # 080AD041 = EISAID(PNP0A08)
        ]
        for mtxt in must:
            if mtxt not in flat:
                errors.append(f"acpiexec output lacks {mtxt!r}")
        if "Interrupt" not in out or "00000021" not in out:
            errors.append("UART interrupt 33 missing from _CRS")
        if "--verbose" in sys.argv:
            print(out)
    if errors:
        print("ACPI check FAILED:")
        for e in errors:
            print("  " + e)
        return 1
    print("ACPI check: tables, loader script and AML OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
