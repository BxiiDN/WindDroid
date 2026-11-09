/*
 * WindDroid Monitor - interactive demo guest bundled with the Android app.
 * Runs at EL1 on the virtual machine and talks over the PL011 UART.
 */
#include "rt.h"

#define VERSION "0.1.1"

/* ---------------------------------------------------------------- SHA-256 */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256(const uint8_t *msg, size_t len, uint32_t out[8])
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8_t blk[64];
    size_t total = ((len + 9 + 63) / 64) * 64;
    for (size_t off = 0; off < total; off += 64) {
        for (int i = 0; i < 64; i++) {
            size_t p = off + (size_t)i;
            uint8_t b;
            if (p < len) b = msg[p];
            else if (p == len) b = 0x80;
            else if (p >= total - 8) b = (uint8_t)(((uint64_t)len * 8) >> (8 * (total - 1 - p)));
            else b = 0;
            blk[i] = b;
        }
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)blk[4 * i] << 24 | (uint32_t)blk[4 * i + 1] << 16 | (uint32_t)blk[4 * i + 2] << 8 | blk[4 * i + 3];
        for (int i = 16; i < 64; i++)
            w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
                   (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10));
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    memcpy(out, h, sizeof h);
}

/* ---------------------------------------------------------------- interrupts */

static volatile uint64_t ticks, sgi_count, svc_count;

static int irq_hook(struct frame *f, int kind)
{
    if (kind == 4 && (READ_SYS(esr_el1) >> 26) == 0x15) {   /* SVC used by selftest */
        svc_count++;
        f->x[0] += 1;
        return 1;
    }
    if (kind != 5) return 0;
    uint64_t iar = READ_SYS(s3_0_c12_c12_0);
    uint32_t id = (uint32_t)iar & 0xFFFFFF;
    if (id == 1023) return 1;
    if (id == 27) { ticks++; WRITE_SYS(cntv_ctl_el0, 0); }
    else if (id == 2) sgi_count++;
    else if (id == 33) mmio_w32(UART_BASE + 0x38, 0);   /* mask RX until the main loop drains it */
    WRITE_SYS(s3_0_c12_c12_1, iar);
    return 1;
}

static void gic_init(void)
{
    mmio_w32(GICR_BASE + 0x14, mmio_r32(GICR_BASE + 0x14) & ~2u);
    while (mmio_r32(GICR_BASE + 0x14) & 4) {}
    mmio_w32(GICR_BASE + 0x10000 + 0x80, 0xFFFFFFFFu);
    for (int i = 0; i < 32; i += 4) mmio_w32(GICR_BASE + 0x10000 + 0x400 + i, 0x80808080u);
    mmio_w32(GICR_BASE + 0x10000 + 0x100, (1u << 27) | (1u << 2));
    mmio_w32(GICD_BASE, 3);
    mmio_w32(GICD_BASE + 0x84, 0xFFFFFFFFu);
    *(volatile uint8_t *)(GICD_BASE + 0x400 + 33) = 0xA0;
    mmio_w32(GICD_BASE + 0x104, 1u << 1);
    WRITE_SYS(s3_0_c12_c12_5, 7);
    ISB();
    WRITE_SYS(s3_0_c4_c6_0, 0xF0);
    WRITE_SYS(s3_0_c12_c12_7, 1);
    ISB();
    mmio_w32(UART_BASE + 0x38, (1u << 4) | (1u << 6));
    exc_hook = irq_hook;
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

/* ---------------------------------------------------------------- helpers */

static uint64_t now_ms(void) { return READ_SYS(cntvct_el0) / (READ_SYS(cntfrq_el0) / 1000); }

static uint64_t parse_u(const char *s, uint64_t def)
{
    while (*s == ' ') s++;
    if (!*s) return def;
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint64_t)(*s++ - '0');
    return v;
}

static int readline(char *buf, int max)
{
    int n = 0;
    for (;;) {
        int c = getc_nb();
        if (c < 0) {
            mmio_w32(UART_BASE + 0x38, (1u << 4) | (1u << 6));   /* wake on input */
            __asm__ volatile("wfi");
            continue;
        }
        if (c == '\r' || c == '\n') { puts_("\n"); buf[n] = 0; return n; }
        if ((c == 8 || c == 127) && n > 0) { n--; puts_("\b \b"); continue; }
        if (c >= 32 && c < 127 && n < max - 1) { buf[n++] = (char)c; putc_((char)c); }
    }
}

/* ---------------------------------------------------------------- commands */

static void cmd_help(void)
{
    puts_("Commands:\n"
          "  help            this list\n"
          "  info            virtual CPU and machine information\n"
          "  selftest        check timer, GIC, exceptions, atomics, CRC32, SHA-256\n"
          "  bench           SHA-256 throughput and emulator speed\n"
          "  primes N        list primes below N (default 100)\n"
          "  mandel          ASCII Mandelbrot set (integer math)\n"
          "  uptime          time since boot\n"
          "  echo TEXT       print TEXT\n"
          "  clear           clear the screen\n"
          "  reboot          PSCI SYSTEM_RESET\n"
          "  poweroff        PSCI SYSTEM_OFF\n");
}

static void cmd_info(void)
{
    printf_("CPU      : AArch64, MIDR %08llx, EL%llu\n", (unsigned long long)READ_SYS(midr_el1),
            (unsigned long long)(READ_SYS(CurrentEL) >> 2));
    printf_("Features : ISAR0 %08llx (CRC32, LSE atomics), PFR0 %08llx\n",
            (unsigned long long)READ_SYS(id_aa64isar0_el1), (unsigned long long)READ_SYS(id_aa64pfr0_el1));
    printf_("MMU      : MMFR0 %08llx (4 KiB pages, 40-bit PA)\n", (unsigned long long)READ_SYS(id_aa64mmfr0_el1));
    printf_("RAM      : %llu MiB at 0x40000000\n", (unsigned long long)(mmio_r64(CTL_BASE + 0x18) >> 20));
    printf_("Timer    : %llu Hz generic timer\n", (unsigned long long)READ_SYS(cntfrq_el0));
    printf_("GIC      : v%u, %u interrupt lines\n", (mmio_r32(GICD_BASE + 0xFFE8) >> 4) & 0xF,
            ((mmio_r32(GICD_BASE + 4) & 0x1F) + 1) * 32);
    printf_("Executed : %llu instructions\n", (unsigned long long)icount());
}

static int sw_crc_ok(void)
{
    const uint8_t *s = (const uint8_t *)"123456789";
    uint32_t a = 0xFFFFFFFF, b = 0xFFFFFFFF;
    for (int i = 0; i < 9; i++) {
        a ^= s[i];
        for (int k = 0; k < 8; k++) a = (a >> 1) ^ (0xEDB88320 & (0u - (a & 1)));
        b = __builtin_arm_crc32b(b, s[i]);
    }
    return ~a == 0xCBF43926 && ~b == 0xCBF43926;
}

static void cmd_selftest(void)
{
    int ok = 0, n = 0;
    /* 1 timer */
    uint64_t t = ticks;
    WRITE_SYS(cntv_tval_el0, 6250);
    WRITE_SYS(cntv_ctl_el0, 1);
    uint64_t deadline = now_ms() + 2000;
    while (ticks == t && now_ms() < deadline) __asm__ volatile("wfi");
    n++; if (ticks != t) ok++; else puts_("  timer interrupt: FAILED\n");
    /* 2 SGI */
    uint64_t s = sgi_count;
    WRITE_SYS(s3_0_c12_c11_5, (2ULL << 24) | 1);
    ISB();
    for (volatile int i = 0; i < 1000 && sgi_count == s; i++) {}
    n++; if (sgi_count != s) ok++; else puts_("  SGI: FAILED\n");
    /* 3 SVC round trip */
    register uint64_t x0 __asm__("x0") = 41;
    __asm__ volatile("svc #5" : "+r"(x0) :: "memory");
    n++; if (x0 == 42) ok++; else puts_("  SVC: FAILED\n");
    /* 4 LSE atomics */
    static uint64_t a = 1;
    uint64_t old;
    __asm__ volatile("ldadd %1, %0, [%2]" : "=r"(old) : "r"(9ULL), "r"(&a) : "memory");
    n++; if (old == 1 && a == 10) ok++; else puts_("  atomics: FAILED\n");
    /* 5 CRC32 */
    n++; if (sw_crc_ok()) ok++; else puts_("  CRC32: FAILED\n");
    /* 6 SHA-256 */
    uint32_t h[8];
    sha256((const uint8_t *)"abc", 3, h);
    n++; if (h[0] == 0xba7816bf && h[7] == 0xf20015ad) ok++; else puts_("  SHA-256: FAILED\n");
    if (ok == n) printf_("selftest: all %d checks passed\n", n);
    else printf_("selftest: %d of %d checks passed\n", ok, n);
}

static void cmd_bench(void)
{
    static uint8_t buf[65536];
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)i;
    uint32_t h[8];
    sha256((const uint8_t *)"abc", 3, h);
    printf_("sha256(abc) = %08x%08x...%08x\n", h[0], h[1], h[7]);
    uint64_t i0 = icount(), n0 = mmio_r64(CTL_BASE + 0x10);
    sha256(buf, sizeof buf, h);
    uint64_t i1 = icount(), n1 = mmio_r64(CTL_BASE + 0x10);
    uint64_t insns = i1 - i0, ns = n1 - n0;
    if (!ns) ns = 1;
    uint64_t kbps = (uint64_t)sizeof buf * 1000000ULL / ns;   /* KiB-ish per second */
    printf_("sha256 64 KiB: %llu instructions, %llu.%03llu ms host time\n", (unsigned long long)insns,
            (unsigned long long)(ns / 1000000), (unsigned long long)(ns / 1000 % 1000));
    printf_("speed: %llu MIPS, %llu KB/s\n", (unsigned long long)(insns * 1000 / ns),
            (unsigned long long)kbps);
}

static void cmd_primes(uint64_t n)
{
    if (n > 100000) n = 100000;
    int col = 0;
    for (uint64_t i = 2; i < n; i++) {
        int p = 1;
        for (uint64_t d = 2; d * d <= i; d++) if (i % d == 0) { p = 0; break; }
        if (!p) continue;
        printf_("%llu ", (unsigned long long)i);
        if (++col == 16) { puts_("\n"); col = 0; }
    }
    puts_("\n");
}

static void cmd_mandel(void)
{
    const char *shades = " .:-=+*#%@";
    for (int y = 0; y < 22; y++) {
        for (int x = 0; x < 64; x++) {
            int64_t cr = -2 * 4096 + x * (3 * 4096) / 64, ci = -1 * 4096 + y * (2 * 4096) / 22;
            int64_t zr = 0, zi = 0;
            int it = 0;
            while (it < 30 && zr * zr + zi * zi < 4LL * 4096 * 4096) {
                int64_t t = (zr * zr - zi * zi) / 4096 + cr;
                zi = 2 * zr * zi / 4096 + ci;
                zr = t;
                it++;
            }
            putc_(shades[it >= 30 ? 9 : it % 9]);
        }
        puts_("\n");
    }
}

static void hvc0(uint64_t fn)
{
    register uint64_t x0 __asm__("x0") = fn;
    __asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
}

int main(void)
{
    gic_init();
    puts_("\n  __    __ _           _ ___           _     _\n"
          " / / /\\ \\ (_)_ __   __| /   \\_ __ ___ (_) __| |\n"
          " \\ \\/  \\/ / | '_ \\ / _` / /\\ / '__/ _ \\| |/ _` |\n"
          "  \\  /\\  /| | | | | (_| / /_//| | | (_) | | (_| |\n"
          "   \\/  \\/ |_|_| |_|\\__,_/___,' |_|  \\___/|_|\\__,_|\n\n");
    printf_("WindDroid Monitor %s - running on the WindDroid virtual AArch64 machine\n", VERSION);
    puts_("Type 'help' for commands.\n");

    char line[128];
    for (;;) {
        puts_("wd> ");
        readline(line, sizeof line);
        char *cmd = line;
        while (*cmd == ' ') cmd++;
        if (!*cmd) continue;
        if (!strcmp(cmd, "help")) cmd_help();
        else if (!strcmp(cmd, "info")) cmd_info();
        else if (!strcmp(cmd, "selftest")) cmd_selftest();
        else if (!strcmp(cmd, "bench")) cmd_bench();
        else if (!strncmp(cmd, "primes", 6)) cmd_primes(parse_u(cmd + 6, 100));
        else if (!strcmp(cmd, "mandel")) cmd_mandel();
        else if (!strcmp(cmd, "uptime")) {
            uint64_t ms = now_ms();
            printf_("up %llu.%03llu s\n", (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000));
        } else if (!strncmp(cmd, "echo", 4)) printf_("%s\n", cmd[4] ? cmd + 5 : "");
        else if (!strcmp(cmd, "clear")) puts_("\x1b[2J\x1b[H");
        else if (!strcmp(cmd, "reboot")) { puts_("Rebooting...\n"); hvc0(0x84000009); }
        else if (!strcmp(cmd, "poweroff")) { puts_("Powering off.\n"); hvc0(0x84000008); }
        else printf_("unknown command: %s (try 'help')\n", cmd);
    }
}
