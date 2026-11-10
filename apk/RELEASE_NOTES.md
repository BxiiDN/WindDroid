## WindDroid v0.2.0 — mesin virtual UEFI untuk Windows on ARM

### Baru
- **Firmware UEFI asli (EDK2)** berjalan di dalam aplikasi: menu boot, Setup, UEFI Shell, NVRAM tersimpan
- **Layar** (ramfb/GOP) penuh, sentuhan sebagai **tablet USB**, keyboard fisik dan keyboard layar sebagai **keyboard USB**, bilah tombol (Esc, Ctrl, Alt, Win, Del, panah, Ctrl+Alt+Del, Shift+F10…)
- **Disk sistem NVMe** dari file **VHDX**, VHD, atau IMG (misalnya VHDX Windows 11 ARM64 dari Microsoft) — tidak dimuat ke RAM
- **ISO installer** sebagai **USB DVD** (boot El Torito seperti media Windows)
- **Buat disk baru**: VHDX dinamis 32–256 GB
- **JIT ARM64→ARM64**: di tablet ARM64 blok instruksi diterjemahkan ke kode mesin, ±2,4× lebih cepat dari interpreter
- CPU: FP dan AdvSIMD (NEON) lengkap; GICv3 ITS + MSI-X; tabel ACPI termasuk IORT
- RAM VM 1–8 GB

### Pengujian
Semua tes dijalankan ulang di GitHub Actions (x86-64 dan ARM64 asli) lalu APK ini dipasang di emulator Android 14: aplikasi mem-boot EDK2, menampilkan layar 800×600, dan keyboard USB membuka menu boot. Rinciannya di README (108.000+ vektor FP/SIMD, tes firmware dengan UEFI Shell, sampel VHDX asli Hyper-V, JIT diuji di ARM64 asli, 64 bug suntikan harus tertangkap).

### Batasan
- **Boot Windows sendiri belum diuji**. Dengan JIT kecepatannya sekitar 300 juta instruksi per detik (di server ARM64; tablet bisa lebih lambat), masih jauh dari kecepatan asli: Windows akan sangat lambat (instalasi bisa berjam-jam) bila berhasil.
- Belum ada jaringan dan baru satu inti CPU.
- Image VHDX/VHD diferensial (punya parent) belum didukung.

### Cara pakai singkat
1. Salin ISO Windows 11 ARM64 atau VHDX ke penyimpanan internal tablet.
2. **ISO installer…** pilih ISO, **Buat disk baru…** (mis. 64 GB), atau **Disk sistem…** pilih VHDX yang sudah ada.
3. Atur **RAM VM** (4 GB disarankan), tekan **▶ Mulai VM**.

### Instalasi
Unduh `WindDroid-*.apk` di bawah dan pasang. `arm64-v8a` dan `x86_64`, Android 8.0 ke atas.
