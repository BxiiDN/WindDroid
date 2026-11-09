# AArch64 guest programs (tests and the demo monitor), built with clang + lld.
CC      := clang
ARCH    := --target=aarch64-none-elf -march=armv8.1-a+crc+rcpc -mgeneral-regs-only
CFLAGS  := $(ARCH) -ffreestanding -fno-stack-protector -fno-pic -Wall -Wno-unused-function -Ilib -g
LDFLAGS := -nostdlib -static -fuse-ld=lld -Wl,-T,lib/link.ld -Wl,--build-id=none
B       := build
RT      := lib/start.S lib/rt.c
PY      := python3

PROGS := alu_gen mem_gen t_sys t_mmu t_el0 t_irq t_cprog_O0 t_cprog_O2 t_cprog_Os monitor
all: $(addprefix $(B)/,$(addsuffix .elf,$(PROGS)))

$(B)/gen/alu_gen.S: ../tests/gen_alu.py
	@mkdir -p $(B)/gen
	$(PY) $< $@ 12345 4000
$(B)/gen/mem_gen.S: ../tests/gen_mem.py
	@mkdir -p $(B)/gen
	$(PY) $< $@ 777 2500

$(B)/alu_gen.elf: $(B)/gen/alu_gen.S tests/gen_main.c $(RT) lib/rt.h lib/link.ld
	$(CC) $(CFLAGS) -O2 -DTEST_NAME='"alu"' $(LDFLAGS) -o $@ $(RT) tests/gen_main.c $<
$(B)/mem_gen.elf: $(B)/gen/mem_gen.S tests/gen_main.c $(RT) lib/rt.h lib/link.ld
	$(CC) $(CFLAGS) -O2 -DTEST_NAME='"mem"' $(LDFLAGS) -o $@ $(RT) tests/gen_main.c $<

$(B)/t_%.elf: tests/t_%.c $(RT) lib/rt.h lib/link.ld
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -O2 $(LDFLAGS) -o $@ $(RT) $<

$(B)/t_cprog_O0.elf: tests/t_cprog.c $(RT) lib/rt.h lib/link.ld
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -O0 $(LDFLAGS) -o $@ $(RT) $<
$(B)/t_cprog_O2.elf: tests/t_cprog.c $(RT) lib/rt.h lib/link.ld
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -O2 $(LDFLAGS) -o $@ $(RT) $<
$(B)/t_cprog_Os.elf: tests/t_cprog.c $(RT) lib/rt.h lib/link.ld
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -Os $(LDFLAGS) -o $@ $(RT) $<

$(B)/monitor.elf: monitor/monitor.c $(RT) lib/rt.h lib/link.ld
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -O2 $(LDFLAGS) -o $@ $(RT) $<

clean:
	rm -rf $(B)

.PHONY: all clean
