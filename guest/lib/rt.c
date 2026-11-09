/* WindDroid guest runtime: console, formatting, exit, exception dispatch. */
#include "rt.h"

int test_failures;
int (*exc_hook)(struct frame *f, int kind);

void putc_(char c)
{
    mmio_w32(UART_BASE, (uint8_t)c);
}

void puts_(const char *s)
{
    while (*s) putc_(*s++);
}

int getc_nb(void)
{
    if (mmio_r32(UART_BASE + 0x18) & 0x10) return -1;
    return (int)(mmio_r32(UART_BASE) & 0xFF);
}

__attribute__((noreturn)) void wd_exit(int code)
{
    mmio_w32(CTL_BASE, (uint32_t)code);
    for (;;) __asm__ volatile("wfi");
}

uint64_t icount(void) { return mmio_r64(CTL_BASE + 8); }

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    while (n--) *a++ = *b++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ formatting */

struct out { char *buf; size_t n, pos; };

static void emit(struct out *o, char c)
{
    if (o->buf) { if (o->pos + 1 < o->n) o->buf[o->pos] = c; }
    else putc_(c);
    o->pos++;
}

static void emit_num(struct out *o, unsigned long long v, int base, int upper, int width, int zero, int neg)
{
    char tmp[32];
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do { tmp[n++] = dig[v % (unsigned)base]; v /= (unsigned)base; } while (v);
    int len = n + neg;
    if (!zero) while (width-- > len) emit(o, ' ');
    if (neg) emit(o, '-');
    if (zero) while (width-- > len) emit(o, '0');
    while (n) emit(o, tmp[--n]);
}

static int vformat(struct out *o, const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f != '%') { emit(o, *f); continue; }
        f++;
        int zero = 0, width = 0, lng = 0;
        if (*f == '0') { zero = 1; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        while (*f == 'l') { lng++; f++; }
        if (*f == 'z') { lng = 2; f++; }
        switch (*f) {
        case 'd': case 'i': {
            long long v = lng ? va_arg(ap, long long) : va_arg(ap, int);
            emit_num(o, v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v, 10, 0, width, zero, v < 0);
            break;
        }
        case 'u': {
            unsigned long long v = lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
            emit_num(o, v, 10, 0, width, zero, 0);
            break;
        }
        case 'x': case 'X': {
            unsigned long long v = lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
            emit_num(o, v, 16, *f == 'X', width, zero, 0);
            break;
        }
        case 'p':
            emit(o, '0'); emit(o, 'x');
            emit_num(o, (unsigned long long)(uintptr_t)va_arg(ap, void *), 16, 0, 16, 1, 0);
            break;
        case 'c': emit(o, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            while (width-- > len) emit(o, ' ');
            while (*s) emit(o, *s++);
            break;
        }
        case '%': emit(o, '%'); break;
        default: emit(o, '%'); emit(o, *f); break;
        }
    }
    if (o->buf && o->n) o->buf[o->pos < o->n ? o->pos : o->n - 1] = 0;
    return (int)o->pos;
}

int vsnprintf_(char *buf, size_t n, const char *fmt, va_list ap)
{
    struct out o = { buf, n, 0 };
    return vformat(&o, fmt, ap);
}

int snprintf_(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf_(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int printf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    struct out o = { 0, 0, 0 };
    int r = vformat(&o, fmt, ap);
    va_end(ap);
    return r;
}

/* ------------------------------------------------------------------ exceptions */

void exc_handler(struct frame *f, int kind)
{
    if (exc_hook && exc_hook(f, kind)) return;
    uint64_t esr = READ_SYS(esr_el1), far = READ_SYS(far_el1);
    printf_("\nPANIC: unexpected exception kind=%d esr=%llx (EC=%llx) far=%llx elr=%llx\n", kind,
            (unsigned long long)esr, (unsigned long long)(esr >> 26), (unsigned long long)far,
            (unsigned long long)f->elr);
    wd_exit(99);
}
