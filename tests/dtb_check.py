#!/usr/bin/env python3
"""Check the device tree WindDroid hands to the firmware: one cpu@N node per
virtual CPU (reg = MPIDR Aff0, PSCI enable method) and a GICv3 redistributor
region with one 128 KiB frame per CPU.

Usage: dtb_check.py VM FIRMWARE
"""
import os
import struct
import subprocess
import sys
import tempfile


def parse(blob):
    """Returns {path: {prop: bytes}} for a flattened device tree."""
    magic, total, off_struct, off_strings = struct.unpack_from(">IIII", blob, 0)
    assert magic == 0xD00DFEED, "bad magic"
    nodes, stack, p = {}, [], off_struct

    def string(o):
        return blob[off_strings + o:blob.index(b"\0", off_strings + o)].decode()

    while True:
        tok = struct.unpack_from(">I", blob, p)[0]
        p += 4
        if tok == 1:                                   # BEGIN_NODE
            end = blob.index(b"\0", p)
            stack.append(blob[p:end].decode())
            p = (end + 4) & ~3
            nodes["/" + "/".join(stack[1:])] = {}
        elif tok == 2:                                 # END_NODE
            stack.pop()
        elif tok == 3:                                 # PROP
            ln, name = struct.unpack_from(">II", blob, p)
            p += 8
            nodes["/" + "/".join(stack[1:])][string(name)] = blob[p:p + ln]
            p = (p + ln + 3) & ~3
        elif tok == 4:
            continue
        elif tok == 9:
            return nodes
        else:
            raise ValueError(f"bad token {tok}")


def check(vm, fw, ncpu):
    errors = []
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "x.dtb")
        subprocess.run([vm, "-q", "-P", str(ncpu), "-b", fw, "-d", path, "-n", "1000"], capture_output=True)
        nodes = parse(open(path, "rb").read())
    cpus = sorted(k for k in nodes if k.startswith("/cpus/cpu@"))
    if cpus != [f"/cpus/cpu@{k}" for k in range(ncpu)]:
        errors.append(f"{ncpu} CPUs: cpu nodes {cpus}")
    for k in range(ncpu):
        n = nodes.get(f"/cpus/cpu@{k}", {})
        if n.get("reg") != struct.pack(">I", k) or n.get("enable-method") != b"psci\0" \
                or n.get("device_type") != b"cpu\0":
            errors.append(f"{ncpu} CPUs: cpu@{k} properties wrong: {n}")
    if nodes.get("/psci", {}).get("cpu_on") != struct.pack(">I", 0xC4000003):
        errors.append("psci node lacks cpu_on")
    gic = nodes.get("/intc@8000000", {})
    reg = struct.unpack(">8I", gic.get("reg", b"\0" * 32))
    want = (0, 0x08000000, 0, 0x10000, 0, 0x080A0000, 0, 0x20000 * ncpu)
    if reg != want:
        errors.append(f"{ncpu} CPUs: GIC reg {[hex(x) for x in reg]}")
    return errors


def main():
    vm, fw = sys.argv[1:3]
    errors = []
    for n in (1, 2, 4, 8):
        errors += check(vm, fw, n)
    if errors:
        print("DTB check FAILED:")
        for e in errors:
            print("  " + e)
        return 1
    print("DTB check: cpu nodes and GIC redistributor region OK for 1, 2, 4, 8 CPUs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
