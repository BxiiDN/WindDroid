# WindDroid engine: host build and test suite.
#   make            build build/winddroid-vm for this machine
#   make test       build the guest test programs and run every test
#   make guests     build only the AArch64 guest programs

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Iinclude
LDFLAGS ?=

ENGINE_SRC := core/cpu.c core/mmu.c core/sysreg.c devices/gic.c devices/simple.c host/machine.c
ENGINE_HDR := include/winddroid.h core/internal.h

BUILD := build

all: $(BUILD)/winddroid-vm

$(BUILD)/winddroid-vm: $(ENGINE_SRC) host/main.c $(ENGINE_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(ENGINE_SRC) host/main.c $(LDFLAGS)

$(BUILD)/unit_tests: $(ENGINE_SRC) tests/unit_tests.c $(ENGINE_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Icore -o $@ $(ENGINE_SRC) tests/unit_tests.c $(LDFLAGS)

guests:
	$(MAKE) -C guest

test: $(BUILD)/winddroid-vm $(BUILD)/unit_tests guests
	$(BUILD)/unit_tests
	python3 tests/run_tests.py

clean:
	rm -rf $(BUILD)
	$(MAKE) -C guest clean

.PHONY: all guests test clean
