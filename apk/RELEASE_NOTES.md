## WindDroid — mesin virtual AArch64 untuk Android

Rilis pertama engine WindDroid dan aplikasi Android-nya.

### Isi
- Interpreter CPU AArch64 (EL0/EL1): instruksi integer ARMv8.0, LSE atomics, CRC32, exception, PAN
- MMU stage 1 (4 KiB, TTBR0/TTBR1, blok dan halaman, fault lengkap)
- Mesin dengan peta memori QEMU `virt`: GICv3, timer generik, UART PL011, RTC PL031, PSCI
- Aplikasi Android: jalankan program demo **WindDroid Monitor** atau muat ELF/biner AArch64 sendiri, pilihan RAM 128 MB – 2 GB

### Pengujian
Suite tes dijalankan di x86-64 **dan ARM64 asli**, lalu APK ini dipasang di emulator Android 14 dan program monitor dijalankan lewat aplikasi (selftest, bench, poweroff) sebelum dirilis.

Lebih dari 8.000 instruksi acak dibandingkan dengan model referensi, tes MMU/EL0/interupsi, program C dengan vektor uji SHA-256 resmi, dan uji mutasi (18 dari 18 bug yang disuntikkan tertangkap). Semua tes dijalankan ulang otomatis di GitHub Actions sebelum APK ini dibangun.

### Batasan
**Belum bisa mem-boot Windows.** Masih dibutuhkan FP/SIMD, firmware UEFI + ACPI, penyimpanan, layar, USB, dan JIT. Lihat README untuk peta jalannya.

### Instalasi
Unduh `WindDroid-*.apk` di bawah dan pasang (izinkan instalasi dari sumber tidak dikenal). Mendukung `arm64-v8a` dan `x86_64`, Android 8.0 ke atas.
