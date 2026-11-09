/* Harness for generated instruction tests (alu_gen.S, mem_gen.S). */
#include "rt.h"

extern void run_cases(void);
extern const uint64_t case_count;
extern const char *const case_names[];
static int fails;

void check(uint64_t i, uint64_t got, uint64_t exp, uint64_t gotf, uint64_t expf, uint64_t flags)
{
    int bad = got != exp || (flags && (gotf & 0xF0000000ULL) != expf);
    if (!bad) return;
    fails++;
    if (fails <= 25)
        printf_("FAIL #%llu [%s]: got %llx want %llx, nzcv %llx want %llx\n", (unsigned long long)i,
                case_names[i], (unsigned long long)got, (unsigned long long)exp,
                (unsigned long long)(gotf >> 28), (unsigned long long)(expf >> 28));
}

int main(void)
{
    run_cases();
    printf_("%s: %llu checks, %d failures\n", TEST_NAME, (unsigned long long)case_count, fails);
    return fails ? 1 : 0;
}
