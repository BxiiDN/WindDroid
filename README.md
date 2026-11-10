# WindDroid

Mesin virtual **AArch64** yang ditulis dari nol untuk Android, dengan tujuan menjalankan **Windows on ARM** di tablet Android (target utama: Xiaomi Pad 6, Snapdragon 870).

Pendekatannya sama dengan [Henge](https://github.com/Leviidev/Henge) (macOS di iPhone/iPad): aplikasi Android tidak boleh memakai hypervisor (EL2 sudah dipakai firmware Qualcomm), jadi WindDroid membawa CPU, MMU, dan perangkatnya sendiri. Peta memori dan perangkatnya mengikuti mesin `virt` milik QEMU, konfigurasi yang dipakai orang untuk menjalankan Windows 11 ARM64 di QEMU (UEFI EDK2, `ramfb`, xHCI dengan keyboard/tablet USB, ISO lewat USB, NVMe).

> **Status jujur (v0.2.0):** firmware UEFI asli (EDK2) sudah boot penuh di dalam WindDroid: menu boot, Setup, UEFI Shell, membaca/menulis disk NVMe dan USB, boot dari ISO lewat USB DVD, layar GOP, keyboard USB. Tabel ACPI, ITS/MSI-X, dan perangkat yang dibutuhkan Windows sudah ada dan diuji. **Boot Windows sendiri belum pernah diuji** (installer Windows tidak tersedia di lingkungan pengembangan). Di ARM64 (tablet Android) blok instruksi diterjemahkan langsung ke kode mesin (**JIT**, ±300 juta instruksi/detik di server ARM64, 2,4× interpreter); itu masih jauh dari kecepatan asli, jadi Windows akan lambat. Lihat [Peta jalan](#peta-jalan).

## Isi repo

| Folder | Isi |
|---|---|
| `core/` | Interpreter CPU A64 (integer, FP, AdvSIMD), **JIT AArch64→AArch64**, MMU stage 1 + TLB, register sistem |
| `devices/` | GICv3 + ITS, PL011, PL031, flash CFI, fw_cfg, PCIe (ECAM, MSI-X), ramfb, NVMe, xHCI + HID + mass storage, image disk |
| `host/` | Bus fisik, device tree, tabel ACPI, PSCI, timer, run loop, CLI `winddroid-vm` |
| `include/` | API publik engine (`winddroid.h`) |
| `firmware/` | EDK2 ArmVirtQemu `QEMU_EFI.fd`, `QEMU_VARS.fd`, UEFI Shell (BSD-2-Clause-Patent) |
| `guest/` | Program tamu AArch64: tes, dan **WindDroid Monitor** (demo) |
| `tests/` | Generator tes acak + model referensi, unit test, tes image disk, tes firmware, uji mutasi |
| `apk/` | Aplikasi Android (Java + JNI, tanpa AndroidX) |

## Yang sudah didukung

**CPU (EL0 + EL1)**: semua instruksi integer ARMv8.0 + LSE atomics, CRC32, RCpc; **FP dan AdvSIMD (NEON) lengkap** dengan semantik ARM yang persis (NaN, FPCR.DN/FZ, mode pembulatan, estimasi FRECPE/FRSQRTE, trap CPACR); exception, PAN, MMU 4 KiB dengan fault yang benar.

**Platform (peta memori QEMU `virt`)**

| Alamat | Perangkat |
|---|---|
| `0x00000000` | Flash CFI 64 MiB: firmware UEFI (bank 0) dan NVRAM (bank 1) |
| `0x08000000` | GICv3 distributor, `0x08080000` **ITS**, `0x080A0000` redistributor (LPI) |
| `0x09000000` | UART PL011, `0x09010000` RTC PL031, `0x09020000` fw_cfg (MMIO + DMA) |
| `0x10000000` | Jendela MMIO PCIe, `0x3F000000` ECAM, `0x8000000000` MMIO 64-bit |
| `0x40000000` | RAM |

- **UEFI**: EDK2 `QEMU_EFI.fd` resmi, device tree, fw_cfg dengan `etc/table-loader` (ACPI: DSDT, FADT, MADT dengan ITS, **IORT**, GTDT, MCFG, SPCR, DBG2), `ramfb` → GOP
- **NVMe 1.4** di PCIe (driver bawaan Windows `stornvme`), antrean admin/I-O, PRP, MSI-X per antrean
- **xHCI** (driver bawaan Windows `usbxhci`): keyboard HID, tablet absolut, USB disk, **USB DVD-ROM** (SCSI MMC) untuk ISO
- **Image disk** tanpa memuat ke RAM: raw/IMG, ISO, **VHDX** (dinamis/tetap, sektor 512/4096, replay log, alokasi blok, membuat baru), VHD (tetap/dinamis). Image diferensial (punya parent) ditolak dengan pesan yang jelas.

## Hasil pengujian

`make test` menjalankan semuanya (juga di GitHub Actions pada x86-64 dan ARM64 asli):

| Kelompok | Isi |
|---|---|
| CPU | 4.000 instruksi ALU acak + 4.117 cek load/store vs model Python; **108.000+ vektor FP/SIMD** bit-exact vs referensi `fractions.Fraction`; program C dengan vektor uji SHA-256 resmi |
| Sistem | exception, MMU, EL0, GICv3 + timer + UART IRQ |
| `t_msi` | ITS + LPI + MSI-X dari sisi OS: NVMe (identify, baca/tulis, masking vektor), xHCI (address device, 70 transfer kontrol yang memutar ring) |
| Firmware | EDK2 boot ke menu boot, Setup, NVRAM tersimpan, layar GOP, keyboard USB di menu boot, ACPI divalidasi ACPICA `acpiexec` |
| Disk | UEFI Shell boot dari NVMe (raw dan VHDX), salin file 3 MB, tulis file, mode read-only; boot ISO bergaya Windows dari USB DVD; baca/tulis USB disk |
| Image | sampel asli Hyper-V/Virtual PC/Disk2VHD (dari qemu-iotests), log kotor Hyper-V, uji acak vs cermin RAM, struktur VHDX dicek parser Python independen |

**JIT** (`core/jit.c`, aktif otomatis di host ARM64): semua tes di atas juga dijalankan dengan JIT di runner ARM64 asli, termasuk boot EDK2, UEFI Shell dari NVMe/USB, dan kode yang memodifikasi dirinya sendiri; `tests/jit_diff.py` membandingkan checksum state per instruksi antara interpreter dan JIT.

**Uji mutasi** (`tests/mutation_test.py`): **64 bug** disuntikkan satu per satu (CPU, jalur cepat, FP, SIMD, VHDX, VHD, USB, NVMe, xHCI, ITS, MSI-X, dan JIT di ARM64). Semuanya harus tertangkap sebelum rilis.

## Membangun

```sh
make test                                   # engine + semua tes (clang, lld, python3)

# UEFI Shell dari disk NVMe, keyboard USB, ISO di USB DVD:
build/winddroid-vm -r -m 2048 -b firmware/QEMU_EFI.fd -v vars.fd \
    -D windows.vhdx -c Win11_Arm64.iso -U -I

build/disktool create disk.vhdx 64G         # buat VHDX dinamis kosong
build/disktool info  image.vhdx

cd apk && ./gradlew assembleRelease         # APK (Android SDK + NDK)
```

## Aplikasi Android

- **▶ Mulai VM UEFI / Windows** — layar penuh dengan tampilan GOP; sentuhan = tablet USB (tap = klik kiri, tap dua jari = klik kanan, geser dua jari = gulir); keyboard fisik dan keyboard layar dikirim sebagai keyboard USB; tombol Esc/Tab/Ctrl/Alt/Win/Del/panah/F2/F12/Ctrl+Alt+Del/Shift+F10; konsol serial
- **Disk sistem…** — VHDX/VHD/IMG sebagai SSD NVMe (misalnya VHDX Windows 11 ARM64 Insider dari Microsoft)
- **ISO installer…** — ISO Windows 11 ARM64 sebagai USB DVD
- **Buat disk baru…** — VHDX dinamis 32–256 GB untuk tujuan instalasi
- RAM VM 1–8 GB, NVRAM UEFI tersimpan, **Reset NVRAM**
- **▶ Jalankan Monitor** / **Muat image…** untuk program AArch64 bare-metal

File dibuka lewat pemilih file Android (deskriptor file), jadi image puluhan GB tidak dimuat ke RAM. Simpan image di penyimpanan internal; penyedia cloud yang tidak mendukung akses acak tidak bisa dipakai.

## Peta jalan

1. ~~FP/AdvSIMD~~, ~~UEFI + fw_cfg + ACPI~~, ~~NVMe~~, ~~ramfb~~, ~~xHCI + keyboard/tablet/storage~~, ~~ITS + MSI-X~~
2. ~~JIT arm64→arm64 dasar~~ (instruksi integer, load/store, cabang). Berikutnya: akses memori inline, rantai blok, FP/SIMD di JIT
3. **Multi-core (SMP)**
4. **Jaringan** (USB NCM, driver bawaan Windows) dengan NAT di dalam aplikasi
5. GPU yang lebih cepat dari ramfb, EL2

Referensi: `hw/arm/virt.c` QEMU, GICv3/ITS (ARM IHI 0069), ARM ARM (DDI 0487), NVMe 1.4, xHCI 1.1, USB Mass Storage BOT, SCSI SBC/MMC, [MS-VHDX], spesifikasi VHD, IORT (ARM DEN 0049), ACPI 6.

## Lisensi

GPL-2.0-or-later. Firmware EDK2 di `firmware/`: BSD-2-Clause-Patent (lihat `firmware/EDK2-LICENSE.txt`). Sampel image di `tests/samples/` berasal dari qemu-iotests (GPL-2.0).
