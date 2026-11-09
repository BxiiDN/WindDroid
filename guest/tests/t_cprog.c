/* Compiler-generated code: SHA-256 (FIPS 180-2 vectors), CRC32, sorting, 64/128-bit
 * arithmetic, jump tables, recursion and formatting. Built at -O0, -O2 and -Os. */
#include "rt.h"

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

typedef struct { uint32_t h[8]; uint8_t buf[64]; uint64_t len; unsigned n; } sha256_ctx;

static inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha_block(sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha_init(sha256_ctx *c)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->n = 0;
}

static void sha_update(sha256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    c->len += len;
    while (len--) {
        c->buf[c->n++] = *p++;
        if (c->n == 64) { sha_block(c, c->buf); c->n = 0; }
    }
}

static void sha_final(sha256_ctx *c, char hex[65])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) sha_update(c, &pad, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(c, lb, 8);
    for (int i = 0; i < 8; i++) snprintf_(hex + 8 * i, 9, "%08x", c->h[i]);
}

static void sha_str(const char *s, char hex[65])
{
    sha256_ctx c;
    sha_init(&c);
    sha_update(&c, s, strlen(s));
    sha_final(&c, hex);
}

/* ---------------------------------------------------------------- misc */

static uint32_t crc32_sw(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFF;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
    }
    return ~crc;
}

static uint32_t crc32_hw(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFF;
    while (n >= 8) { uint64_t v; memcpy(&v, p, 8); crc = __builtin_arm_crc32d(crc, v); p += 8; n -= 8; }
    while (n--) crc = __builtin_arm_crc32b(crc, *p++);
    return ~crc;
}

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
static uint64_t xorshift(void)
{
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return rng_state = x;
}

static void sort_u64(uint64_t *a, long lo, long hi)
{
    while (lo < hi) {
        uint64_t p = a[(lo + hi) / 2];
        long i = lo, j = hi;
        while (i <= j) {
            while (a[i] < p) i++;
            while (a[j] > p) j--;
            if (i <= j) { uint64_t t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
        }
        if (j - lo < hi - i) { sort_u64(a, lo, j); lo = i; }
        else { sort_u64(a, i, hi); hi = j; }
    }
}

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static __attribute__((noinline)) int classify(int x)
{
    switch (x) {
    case 0: return 11; case 1: return 23; case 2: return 37; case 3: return 41;
    case 4: return 59; case 5: return 61; case 6: return 79; case 7: return 83;
    case 8: return 97; case 9: return 101; default: return -1;
    }
}

struct big { uint64_t v[9]; };
static __attribute__((noinline)) struct big make_big(uint64_t s)
{
    struct big b;
    for (int i = 0; i < 9; i++) b.v[i] = s * (uint64_t)(i + 1);
    return b;
}

static uint64_t arr[4096];

int main(void)
{
    char hex[65];

    sha_str("abc", hex);
    CHECK_EQ(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), 0);
    sha_str("", hex);
    CHECK_EQ(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), 0);
    sha_str("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", hex);
    CHECK_EQ(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), 0);
    {
        sha256_ctx c;
        sha_init(&c);
        static char block[1000];
        memset(block, 'a', sizeof block);
        for (int i = 0; i < 1000; i++) sha_update(&c, block, sizeof block);
        sha_final(&c, hex);
        CHECK_EQ(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), 0);
    }

    const char *s = "123456789";
    CHECK_EQ(crc32_sw((const uint8_t *)s, 9), 0xCBF43926);
    CHECK_EQ(crc32_hw((const uint8_t *)s, 9), 0xCBF43926);
    static uint8_t data[1000];
    for (int i = 0; i < 1000; i++) data[i] = (uint8_t)(i * 7 + 3);
    CHECK_EQ(crc32_sw(data, 1000), crc32_hw(data, 1000));

    uint64_t sum = 0, xr = 0;
    for (int i = 0; i < 4096; i++) { arr[i] = xorshift(); sum += arr[i]; xr ^= arr[i]; }
    sort_u64(arr, 0, 4095);
    int sorted = 1;
    uint64_t sum2 = 0, xr2 = 0;
    for (int i = 0; i < 4096; i++) { if (i && arr[i - 1] > arr[i]) sorted = 0; sum2 += arr[i]; xr2 ^= arr[i]; }
    CHECK(sorted);
    CHECK_EQ(sum, sum2);
    CHECK_EQ(xr, xr2);

    int bad = 0;
    for (int i = 0; i < 2000; i++) {
        uint64_t n = xorshift(), d = xorshift() >> (xorshift() & 63);
        if (!d) d = 1;
        uint64_t q = n / d, r = n % d;
        if (q * d + r != n || r >= d) bad++;
        int64_t sn = (int64_t)n, sd = (int64_t)(d | 1) * ((i & 1) ? -1 : 1);
        int64_t sq = sn / sd, sr = sn % sd;
        if (sq * sd + sr != sn) bad++;
        unsigned __int128 p = (unsigned __int128)n * d;
        /* schoolbook 64x64->128 with 32-bit halves */
        uint64_t a0 = (uint32_t)n, a1 = n >> 32, b0 = (uint32_t)d, b1 = d >> 32;
        uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
        uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
        uint64_t hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
        uint64_t lo = (mid << 32) | (uint32_t)p00;
        if ((uint64_t)(p >> 64) != hi || (uint64_t)p != lo) bad++;
    }
    CHECK_EQ(bad, 0);

    CHECK_EQ(fib(20), 6765);
    int tsum = 0;
    for (int i = -2; i < 12; i++) tsum += classify(i);
    CHECK_EQ(tsum, 11 + 23 + 37 + 41 + 59 + 61 + 79 + 83 + 97 + 101 - 4);
    struct big b = make_big(3);
    CHECK_EQ(b.v[8], 27);

    char tmp[64];
    snprintf_(tmp, sizeof tmp, "%d|%u|%x|%5s|%03d|%lld", -42, 42u, 0xBEEFu, "ab", 7, -9000000000LL);
    CHECK_EQ(strcmp(tmp, "-42|42|beef|   ab|007|-9000000000"), 0);

    printf_("t_cprog: %d failures\n", test_failures);
    return test_failures ? 1 : 0;
}
