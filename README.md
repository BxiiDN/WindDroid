# WindDroid

Mesin virtual **AArch64** yang ditulis dari nol untuk Android, dengan tujuan jangka panjang menjalankan **Windows on ARM** di tablet Android (target utama: Xiaomi Pad 6, Snapdragon 870).

Pendekatannya sama dengan [Henge](https://github.com/Leviidev/Henge) (macOS di iPhone/iPad): aplikasi Android tidak boleh memakai hypervisor (EL2 sudah dipakai firmware Qualcomm), jadi WindDroid membawa CPU, MMU, dan perangkatnya sendiri. Peta memori dan perangkatnya mengikuti mesin `virt` milik QEMU, yang sudah terbukti bisa mem-boot Windows ARM lewat UEFI.

> **Status jujur (v0.1.1):** engine CPU/MMU/interupsi sudah berjalan dan teruji ketat, dan APK-nya bisa menjalankan program AArch64 bare-metal. **Windows belum bisa di-boot.** Bagian yang masih kurang tercantum di [Peta jalan](#peta-jalan-menuju-windows).

## Isi repo

| Folder | Isi |
|---|---|
| `core/` | Interpreter CPU A64, MMU (translasi stage 1, TLB), register sistem |
| `devices/` | GICv3, UART PL011, RTC PL031, perangkat kontrol WindDroid |
| `host/` | Bus fisik, loader ELF/raw, PSCI, timer generik, run loop, CLI `winddroid-vm` |
| `include/` | API publik engine (`winddroid.h`) |
| `guest/` | Program tamu AArch64: runtime, program uji, dan **WindDroid Monitor** (demo interaktif) |
| `tests/` | Generator tes acak + model referensi Python, unit test, runner, uji mutasi |
| `apk/` | Aplikasi Android (Java + JNI, tanpa AndroidX) |

## Yang sudah didukung

**CPU (EL0 + EL1, AArch64)**
- Seluruh instruksi integer ARMv8.0: aritmetika/logika (imm, shifted, extended), bitfield, EXTR, MADD/MSUB/SMULL/UMULH, UDIV/SDIV, CLZ/CLS/RBIT/REV, CSEL/CCMP, semua cabang
- Load/store semua mode pengalamatan, pasangan (LDP/STP/LDPSW), literal, unprivileged (LDTR/STTR)
- Exclusive (LDXR/STXR/LDXP/STXP), acquire/release, **LSE atomics** (LDADD/LDCLR/LDEOR/LDSET/LDSMAX/…/SWP/CAS/CASP), **CRC32/CRC32C**, LDAPR (RCpc)
- Exception: SVC, BRK, undefined, data/instruction abort, alignment, IRQ; ERET; SP_EL0/SP_EL1; PAN
- Register sistem: SCTLR/TCR/TTBR/MAIR/VBAR/ESR/FAR/ELR/SPSR/PAR, register ID, cache info, DC ZVA, AT, TLBI, timer
- FP/SIMD **belum** — dilaporkan "tidak ada" di `ID_AA64PFR0_EL1` dan menghasilkan exception undefined

**MMU:** granule 4 KiB, TTBR0/TTBR1, T0SZ/T1SZ, blok 1 GiB/2 MiB dan halaman 4 KiB, AP/UXN/PXN/WXN, Access Flag, PAN, TBI, fault dengan level yang benar, TLB 1024 entri

**Mesin (peta memori QEMU `virt`)**

| Alamat | Perangkat |
|---|---|
| `0x08000000` | GICv3 distributor (96 jalur interupsi) |
| `0x080A0000` | GICv3 redistributor (CPU 0) |
| `0x09000000` | UART PL011 (IRQ 33) |
| `0x09010000` | RTC PL031 |
| `0x09100000` | Perangkat kontrol WindDroid (exit code, penghitung instruksi, jam host) |
| `0x40000000` | RAM (128 MB – 2 GB) |

Timer generik 62,5 MHz (PPI 27 virtual, PPI 30 fisik), PSCI 1.0 lewat HVC/SMC (SYSTEM_OFF, SYSTEM_RESET, CPU_ON, FEATURES, …).

## Hasil pengujian

`make test` menjalankan:

| Tes | Isi | Hasil |
|---|---|---|
| `unit_tests` | Decoder logical-immediate dibandingkan dengan **8.192 encoding** hasil disassembler LLVM, loader ELF | lolos |
| `alu_gen` | **4.000** instruksi acak dibandingkan dengan model referensi Python (hasil + flag NZCV) | 0 gagal |
| `mem_gen` | **4.117** cek load/store di semua mode pengalamatan terhadap model memori Python | 0 gagal |
| `t_sys` | Exception, ESR/ELR/FAR, alignment, sysreg, DC ZVA, PSCI, exclusive, LSE atomics | lolos |
| `t_mmu` | Tabel halaman, fault translasi/izin/access-flag di level yang benar, AT, TLBI, eksekusi dari VA tinggi, PXN/UXN, blok 2 MiB | lolos |
| `t_el0` | Masuk user mode, syscall, trap instruksi privileged dari EL0, fault izin, WFI trap, PAN, LDTR/STTR | lolos |
| `t_irq` | GICv3 + timer + SGI + interupsi RX UART + masking prioritas | lolos |
| `t_cprog_O0/O2/Os` | Kode C hasil compiler: SHA-256 (vektor uji FIPS, termasuk 1 juta 'a'), CRC32, quicksort 4.096 elemen, aritmetika 64/128-bit, jump table | lolos |
| `monitor` | Program demo interaktif dijalankan dengan input skrip | lolos |

**Uji mutasi** (`tests/mutation_test.py`): 18 bug sengaja disuntikkan ke engine satu per satu (flag overflow salah, kondisi GE/LT terbalik, halaman read-only bisa ditulis, PAN diabaikan, STXR selalu sukses, dll). **Ke-18 bug tertangkap** oleh suite — ini bukti bahwa tes-tes di atas benar-benar memeriksa perilaku.

Kecepatan interpreter di host x86-64: sekitar 50–100 juta instruksi per detik.

## Membangun

```sh
# engine + semua tes (butuh clang, lld, python3)
make test

# jalankan program tamu
build/winddroid-vm guest/build/monitor.elf
build/winddroid-vm -i 'help\nbench\n' guest/build/monitor.elf

# APK (butuh Android SDK + NDK)
cd apk && ./gradlew assembleRelease
```

APK dibangun otomatis oleh GitHub Actions di setiap push, dan setiap tag `v*` menghasilkan **Release** berisi APK yang sudah ditandatangani.

## Aplikasi Android

- **▶ Jalankan Monitor**: menyalakan mesin virtual dengan program demo bawaan (perintah `help`, `info`, `selftest`, `bench`, `mandel`, `primes`, `uptime`, `poweroff`)
- **Muat image…**: pilih ELF AArch64 atau biner mentah (dimuat di `0x40000000`)
- Pilihan RAM 128 MB – 2 GB, terminal dengan dukungan dasar VT100, statistik MIPS langsung
- ABI: `arm64-v8a` dan `x86_64`, minimal Android 8.0

## Peta jalan menuju Windows

Windows 11 on ARM butuh banyak hal yang belum ada. Urutan pengerjaan yang direncanakan:

1. **FP/AdvSIMD (NEON)** — wajib; kernel dan aplikasi Windows memakainya di mana-mana
2. **Firmware UEFI** — memuat EDK2 `QEMU_EFI.fd` di flash `0x00000000`, ditambah `fw_cfg` dan **tabel ACPI** (Windows tidak memakai device tree)
3. **Penyimpanan** — virtio-blk atau NVMe di PCIe ECAM, dengan driver Windows ARM64
4. **Layar** — `ramfb` dulu (framebuffer sederhana), lalu GPU yang lebih cepat
5. **USB keyboard/mouse (XHCI) dan jaringan**
6. **Multi-core (SMP)** dan pemakaian EL2 yang diperlukan beberapa fitur keamanan Windows
7. **JIT arm64→arm64** seperti Henge, menggantikan interpreter, untuk kecepatan yang layak

Referensi yang dipakai: peta memori dan perilaku perangkat dari `hw/arm/virt.c` di QEMU, spesifikasi GICv3 (ARM IHI 0069), dan ARM Architecture Reference Manual (DDI 0487).

## Lisensi

GPL-2.0-or-later.
