## WindDroid v0.3.0 — multi-core dan JIT lebih cepat

### Baru
- **Multi-core (SMP)**: mesin virtual bisa punya 1, 2, atau 4 CPU (tombol **CPU** di layar utama, bawaan 2 inti karena Windows 11 meminta minimal 2 inti). Tiap CPU punya MPIDR, timer, dan redistributor GICv3 sendiri; SGI antar-CPU, PSCI `CPU_ON`/`CPU_OFF`/`AFFINITY_INFO`; device tree dan tabel ACPI (MADT, DSDT) mengikuti jumlah CPU.
- **JIT lebih cepat**: load/store ke RAM dicek di TLB langsung dari kode hasil terjemahan, blok di halaman yang sama dirantai tanpa kembali ke dispatcher, kode yang memodifikasi dirinya sendiri dideteksi per halaman. Program uji C: ±134 → ±448 juta instruksi/detik di ARM64 (±3,3× interpreter).

### Pengujian
- Tes baru `t_smp` dengan 4 CPU: spinlock LDAXR/STXR dan LDADD dari semua CPU (12.000 penambahan tanpa ada yang hilang), STXR wajib gagal bila CPU lain menulis di antaranya, timer per CPU, SGI ke CPU tertentu dan ke semua CPU lain, akses redistributor CPU lain, CPU_OFF.
- EDK2 boot ke menu boot dengan 4 CPU; device tree dicek untuk 1/2/4/8 CPU; MADT dicek per CPU dan divalidasi ACPICA.
- 72 bug suntikan (termasuk 7 untuk SMP dan 7 untuk JIT) semuanya tertangkap; semua tes juga dijalankan dengan JIT di runner ARM64 asli, lalu APK diuji di emulator Android 14.

### Batasan
- **Boot Windows sendiri belum diuji.**
- Semua CPU virtual bergantian di **satu** thread host: menambah CPU membuat Windows mau berjalan (syarat 2 inti), bukan membuatnya lebih cepat.
- Belum ada jaringan. Image VHDX/VHD diferensial belum didukung.

### Cara pakai singkat
1. Salin ISO Windows 11 ARM64 atau VHDX ke penyimpanan internal tablet.
2. **ISO installer…** pilih ISO, **Buat disk baru…** (mis. 64 GB), atau **Disk sistem…** pilih VHDX yang sudah ada.
3. Atur **RAM VM** (4 GB disarankan) dan **CPU** (2 inti), tekan **▶ Mulai VM**.

### Instalasi
Unduh `WindDroid-*.apk` di bawah dan pasang. `arm64-v8a` dan `x86_64`, Android 8.0 ke atas.
