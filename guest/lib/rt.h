/* WindDroid guest runtime API. */
#ifndef WD_RT_H
#define WD_RT_H
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define UART_BASE 0x09000000UL
#define CTL_BASE  0x09100000UL
#define GICD_BASE 0x08000000UL
#define GICR_BASE 0x080A0000UL
#define RTC_BASE  0x09010000UL

struct frame {
    uint64_t x[31];
    uint64_t elr, spsr, sp_el0;
};

static inline void mmio_w32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t mmio_r32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void mmio_w64(uintptr_t a, uint64_t v) { *(volatile uint64_t *)a = v; }
static inline uint64_t mmio_r64(uintptr_t a) { return *(volatile uint64_t *)a; }

#define READ_SYS(r) ({ uint64_t __v; __asm__ volatile("mrs %0, " #r : "=r"(__v)); __v; })
#define WRITE_SYS(r, v) __asm__ volatile("msr " #r ", %0" :: "r"((uint64_t)(v)) : "memory")
#define ISB() __asm__ volatile("isb" ::: "memory")

void putc_(char c);
void puts_(const char *s);
int getc_nb(void);            /* -1 if no input */
int printf_(const char *fmt, ...);
int vsnprintf_(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf_(char *buf, size_t n, const char *fmt, ...);
__attribute__((noreturn)) void wd_exit(int code);
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);

/* exception hook used by tests; return 0 to fall through to the default panic */
extern int (*exc_hook)(struct frame *f, int kind);
uint64_t icount(void);

/* test helpers */
extern int test_failures;
#define CHECK(cond) do { if (!(cond)) { test_failures++; printf_("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b); if (_a != _b) { test_failures++; \
    printf_("FAIL %s:%d: %s == %s (%llx != %llx)\n", __FILE__, __LINE__, #a, #b, (unsigned long long)_a, (unsigned long long)_b); } } while (0)
#endif
