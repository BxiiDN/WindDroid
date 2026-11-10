# WindDroid engine: host build and test suite.
#   make            build build/winddroid-vm for this machine
#   make test       build the guest test programs and run every test
#   make guests     build only the AArch64 guest programs

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -frounding-math -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Iinclude
LDFLAGS ?=
LDLIBS  := -lm

ENGINE_SRC := core/cpu.c core/mmu.c core/sysreg.c core/simd.c core/fpu.c core/jit.c devices/gic.c devices/its.c devices/simple.c devices/flash.c devices/fwcfg.c devices/pci.c devices/ramfb.c devices/disk.c devices/nvme.c devices/image.c devices/usb.c devices/usb_storage.c devices/xhci.c host/machine.c host/platform.c host/fdt.c host/acpi.c
ENGINE_HDR := include/winddroid.h core/internal.h core/fpu.h host/fdt.h host/platform.h

BUILD := build

all: $(BUILD)/winddroid-vm $(BUILD)/disktool

$(BUILD)/winddroid-vm: $(ENGINE_SRC) host/main.c $(ENGINE_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(ENGINE_SRC) host/main.c $(LDFLAGS) $(LDLIBS)

$(BUILD)/unit_tests: $(ENGINE_SRC) tests/unit_tests.c $(ENGINE_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Icore -o $@ $(ENGINE_SRC) tests/unit_tests.c $(LDFLAGS) $(LDLIBS)

$(BUILD)/disktool: $(ENGINE_SRC) tests/disktool.c $(ENGINE_HDR)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(ENGINE_SRC) tests/disktool.c $(LDFLAGS) $(LDLIBS)

guests:
	$(MAKE) -C guest

test: $(BUILD)/winddroid-vm $(BUILD)/unit_tests $(BUILD)/disktool guests
	$(BUILD)/unit_tests
	python3 tests/image_tests.py
	python3 tests/run_tests.py

clean:
	rm -rf $(BUILD)
	$(MAKE) -C guest clean

.PHONY: all guests test clean
