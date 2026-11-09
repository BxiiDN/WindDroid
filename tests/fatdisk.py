#!/usr/bin/env python3
"""
Build and inspect GPT disk images with a FAT32 EFI system partition, written
from the specifications (UEFI 2.10 ch. 5 for GPT, Microsoft FAT32 spec) so
the tests do not depend on mkfs/mtools.

  fatdisk.py create OUT.img SIZE_MB  dest=src ...     (dest like EFI/BOOT/BOOTAA64.EFI)
  fatdisk.py cat IMG PATH                              (print a file from the ESP)
  fatdisk.py ls IMG [DIR]

SPDX-License-Identifier: GPL-2.0-or-later
"""
import struct
import sys
import uuid
import zlib

SECTOR = 512
ESP_TYPE = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
PART_START = 2048                     # first LBA of the ESP (1 MiB aligned)


# ------------------------------------------------------------------ GPT

def guid_bytes(u):
    return u.bytes_le


def gpt(total_sectors, part_first, part_last, disk_guid, part_guid):
    entries = bytearray(128 * 128)
    name = "EFI system partition".encode("utf-16-le")
    entries[0:128] = (guid_bytes(ESP_TYPE) + guid_bytes(part_guid) +
                      struct.pack("<QQQ", part_first, part_last, 0) + name.ljust(72, b"\0"))
    ecrc = zlib.crc32(entries) & 0xFFFFFFFF

    def header(my, alt, entries_lba):
        h = bytearray(struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x00010000, 92, 0, 0,
                                  my, alt, 34, total_sectors - 34, guid_bytes(disk_guid),
                                  entries_lba, 128, 128, ecrc))
        struct.pack_into("<I", h, 16, zlib.crc32(h) & 0xFFFFFFFF)
        return bytes(h).ljust(SECTOR, b"\0")

    mbr = bytearray(SECTOR)
    size = min(total_sectors - 1, 0xFFFFFFFF)
    mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\x00\x02\x00", 0xEE, b"\xff\xff\xff", 1, size)
    mbr[510:512] = b"\x55\xAA"
    primary = bytes(mbr) + header(1, total_sectors - 1, 2) + bytes(entries)
    backup = bytes(entries) + header(total_sectors - 1, 1, total_sectors - 33)
    return primary, backup


# ------------------------------------------------------------------ FAT32

def short_name(name):
    base, _, ext = name.upper().partition(".")
    if len(base) > 8 or len(ext) > 3 or not base:
        raise ValueError("8.3 names only: " + name)
    return base.ljust(8).encode() + ext.ljust(3).encode()


class Fat32:
    def __init__(self, sectors, label=b"WINDDROID  "):
        self.spc = 1                                          # 512-byte clusters
        self.reserved = 32
        self.sectors = sectors
        clusters = sectors // self.spc
        self.fat_sectors = (clusters * 4 + 2 * 4 + SECTOR - 1) // SECTOR
        self.data_start = self.reserved + 2 * self.fat_sectors
        self.nclusters = (sectors - self.data_start) // self.spc
        if self.nclusters < 65525:
            raise ValueError("partition too small for FAT32")
        self.fat = [0] * (self.nclusters + 2)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = 0x0FFFFFFF
        self.data = {}
        self.next_free = 2
        self.label = label
        self.root = self.alloc_chain(b"")
        self.dirs = {"": []}                                 # path -> list of entries
        self.dir_cluster = {"": self.root}

    def alloc_chain(self, payload, min_clusters=1):
        csize = self.spc * SECTOR
        n = max(min_clusters, (len(payload) + csize - 1) // csize)
        first = self.next_free
        for i in range(n):
            c = first + i
            self.fat[c] = c + 1 if i + 1 < n else 0x0FFFFFFF
            self.data[c] = payload[i * csize:(i + 1) * csize].ljust(csize, b"\0")
        self.next_free += n
        return first

    @staticmethod
    def entry(name11, attr, cluster, size):
        return struct.pack("<11sBBBHHHHHHHI", name11, attr, 0, 0, 0x5000, 0x5A21, 0x5A21,
                           cluster >> 16, 0x5000, 0x5A21, cluster & 0xFFFF, size)

    def mkdir(self, path):
        if path in self.dirs:
            return
        parent, _, name = path.rpartition("/")
        self.mkdir(parent)
        c = self.alloc_chain(b"", 8)
        self.dirs[path] = []
        self.dir_cluster[path] = c
        self.dirs[parent].append(self.entry(short_name(name), 0x10, c, 0))

    def add_file(self, path, payload):
        parent, _, name = path.rpartition("/")
        self.mkdir(parent)
        c = self.alloc_chain(payload) if payload else 0
        self.dirs[parent].append(self.entry(short_name(name), 0x20, c, len(payload)))

    def finish(self):
        csize = self.spc * SECTOR
        for path, ents in self.dirs.items():
            c = self.dir_cluster[path]
            body = b""
            if path == "":
                body += self.entry(self.label, 0x08, 0, 0)
            else:
                parent = path.rpartition("/")[0]
                pc = self.dir_cluster[parent] if parent else 0
                body += self.entry(b".          ", 0x10, c, 0) + self.entry(b"..         ", 0x10, pc, 0)
            body += b"".join(ents)
            chain = []
            while True:
                chain.append(c)
                if self.fat[c] >= 0x0FFFFFF8:
                    break
                c = self.fat[c]
            if len(body) > len(chain) * csize:
                raise ValueError("directory too large")
            for i, cl in enumerate(chain):
                self.data[cl] = body[i * csize:(i + 1) * csize].ljust(csize, b"\0")

        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x58\x90"
        bs[3:11] = b"WINDDRD "
        struct.pack_into("<HBHBHHBHHHII", bs, 11, SECTOR, self.spc, self.reserved, 2, 0, 0, 0xF8, 0,
                         63, 255, PART_START, self.sectors)
        struct.pack_into("<IHHIHH", bs, 36, self.fat_sectors, 0, 0, self.root, 1, 6)
        struct.pack_into("<BBBI11s8s", bs, 64, 0x80, 0, 0x29, 0x57444431, self.label, b"FAT32   ")
        bs[510:512] = b"\x55\xAA"
        fsinfo = bytearray(SECTOR)
        struct.pack_into("<I", fsinfo, 0, 0x41615252)
        struct.pack_into("<III", fsinfo, 484, 0x61417272, self.nclusters + 2 - self.next_free, self.next_free)
        struct.pack_into("<I", fsinfo, 508, 0xAA550000)
        fat = struct.pack("<%dI" % len(self.fat), *self.fat).ljust(self.fat_sectors * SECTOR, b"\0")

        out = bytearray(self.data_start * SECTOR)
        out[0:SECTOR] = bs
        out[SECTOR:2 * SECTOR] = fsinfo
        out[6 * SECTOR:7 * SECTOR] = bs
        out[7 * SECTOR:8 * SECTOR] = fsinfo
        out[self.reserved * SECTOR:(self.reserved + self.fat_sectors) * SECTOR] = fat
        out[(self.reserved + self.fat_sectors) * SECTOR:self.data_start * SECTOR] = fat
        used = max(self.data) if self.data else 1
        out += bytes((used - 1) * csize)
        for c, blob in self.data.items():
            off = self.data_start * SECTOR + (c - 2) * csize
            out[off:off + csize] = blob
        return bytes(out)


def create(path, size_mb, files):
    total = size_mb * 1024 * 1024 // SECTOR
    last = total - 34
    part_sectors = last - PART_START + 1
    fs = Fat32(part_sectors)
    for dest, payload in files:
        fs.add_file(dest.strip("/").replace("\\", "/"), payload)
    image = fs.finish()
    primary, backup = gpt(total, PART_START, last, uuid.UUID("57444944-524F-4944-0000-000000000001"),
                          uuid.UUID("57444944-524F-4944-0000-000000000002"))
    with open(path, "wb") as f:
        f.truncate(total * SECTOR)
        f.seek(0)
        f.write(primary)
        f.seek(PART_START * SECTOR)
        f.write(image)
        f.seek((total - 33) * SECTOR)
        f.write(backup)


# ------------------------------------------------------------------ reading (for verifying guest writes)

class Reader:
    def __init__(self, img):
        self.f = open(img, "rb")
        hdr = self.read(SECTOR, SECTOR)
        assert hdr[0:8] == b"EFI PART", "no GPT"
        entries_lba = struct.unpack_from("<Q", hdr, 72)[0]
        ent = self.read(entries_lba * SECTOR, 128)
        self.part = struct.unpack_from("<Q", ent, 32)[0] * SECTOR
        bs = self.read(self.part, SECTOR)
        (self.bps, self.spc, self.res, self.nfats) = struct.unpack_from("<HBHB", bs, 11)
        self.fatsz = struct.unpack_from("<I", bs, 36)[0]
        self.rootc = struct.unpack_from("<I", bs, 44)[0]
        self.data = self.part + (self.res + self.nfats * self.fatsz) * self.bps

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def fat(self, c):
        return struct.unpack("<I", self.read(self.part + self.res * self.bps + 4 * c, 4))[0] & 0x0FFFFFFF

    def chain(self, c):
        csize = self.bps * self.spc
        out = b""
        while 2 <= c < 0x0FFFFFF8:
            out += self.read(self.data + (c - 2) * csize, csize)
            c = self.fat(c)
        return out

    def entries(self, c):
        raw = self.chain(c)
        lfn = []
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = []
                continue
            if e[11] == 0x0F:
                part = e[1:11] + e[14:26] + e[28:32]
                lfn.insert(0, part.decode("utf-16-le", "ignore").split("\0")[0].rstrip("￿"))
                continue
            if e[11] & 0x08:
                lfn = []
                continue
            name = "".join(lfn) if lfn else (e[0:8].decode().rstrip() +
                                             ("." + e[8:11].decode().rstrip() if e[8:11].strip() else ""))
            lfn = []
            cl = struct.unpack_from("<H", e, 20)[0] << 16 | struct.unpack_from("<H", e, 26)[0]
            yield name, e[11], cl, struct.unpack_from("<I", e, 28)[0]

    def lookup(self, path):
        c, size, attr = self.rootc, 0, 0x10
        for comp in [p for p in path.replace("\\", "/").split("/") if p]:
            for name, a, cl, sz in self.entries(c):
                if name.upper() == comp.upper():
                    c, size, attr = cl, sz, a
                    break
            else:
                raise FileNotFoundError(path)
        return c, size, attr

    def cat(self, path):
        c, size, attr = self.lookup(path)
        return self.chain(c)[:size]

    def ls(self, path=""):
        c, _, _ = self.lookup(path)
        return [(n, a, s) for n, a, _, s in self.entries(c) if n not in (".", "..")]


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "create":
        files = []
        for spec in sys.argv[4:]:
            dest, _, src = spec.partition("=")
            files.append((dest, open(src, "rb").read()))
        create(sys.argv[2], int(sys.argv[3]), files)
    elif len(sys.argv) == 4 and sys.argv[1] == "cat":
        sys.stdout.buffer.write(Reader(sys.argv[2]).cat(sys.argv[3]))
    elif len(sys.argv) in (3, 4) and sys.argv[1] == "ls":
        for n, a, s in Reader(sys.argv[2]).ls(sys.argv[3] if len(sys.argv) == 4 else ""):
            print("%-20s %s %d" % (n, "<DIR>" if a & 0x10 else "     ", s))
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main()
