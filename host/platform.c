/*
 * WindDroid - UEFI platform: device tree for the virt machine, firmware
 * flash, fw_cfg contents. The tree follows QEMU's hw/arm/virt.c so that
 * EDK2's ArmVirtQemu finds every device where it expects it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>

#include "../core/internal.h"
#include "fdt.h"
#include "platform.h"

#define PH_GIC  1
#define PH_CLK  2
#define PH_ITS  3

#define GIC_SPI 0
#define GIC_PPI 1
#define IRQ_LEVEL_HI 4

static void build_dtb(wd_machine *m)
{
    fdt f;
    fdt_init(&f);
    fdt_begin(&f, "");
    fdt_prop_u32(&f, "#address-cells", 2);
    fdt_prop_u32(&f, "#size-cells", 2);
    fdt_prop_str(&f, "compatible", "linux,dummy-virt");
    fdt_prop_str(&f, "model", "WindDroid virt");
    fdt_prop_u32(&f, "interrupt-parent", PH_GIC);

    fdt_begin(&f, "chosen");
    fdt_prop_str(&f, "stdout-path", "/pl011@9000000");
    fdt_end(&f);

    fdt_begin(&f, "psci");
    { const char *c[] = { "arm,psci-1.0", "arm,psci-0.2", "arm,psci", NULL }; fdt_prop_strs(&f, "compatible", c); }
    fdt_prop_str(&f, "method", "hvc");
    fdt_prop_u32(&f, "cpu_suspend", 0xC4000001);
    fdt_prop_u32(&f, "cpu_off", 0x84000002);
    fdt_prop_u32(&f, "cpu_on", 0xC4000003);
    fdt_prop_u32(&f, "migrate", 0xC4000005);
    fdt_end(&f);

    fdt_begin(&f, "memory@40000000");
    fdt_prop_str(&f, "device_type", "memory");
    { uint64_t r[] = { WD_RAM_BASE, m->ram_size }; fdt_prop_reg64(&f, "reg", r, 1); }
    fdt_end(&f);

    fdt_begin(&f, "cpus");
    fdt_prop_u32(&f, "#address-cells", 1);
    fdt_prop_u32(&f, "#size-cells", 0);
    fdt_begin(&f, "cpu@0");
    fdt_prop_str(&f, "device_type", "cpu");
    fdt_prop_str(&f, "compatible", "arm,armv8");
    fdt_prop_u32(&f, "reg", 0);
    fdt_prop_str(&f, "enable-method", "psci");
    fdt_end(&f);
    fdt_end(&f);

    fdt_begin(&f, "intc@8000000");
    fdt_prop_u32(&f, "phandle", PH_GIC);
    fdt_prop_str(&f, "compatible", "arm,gic-v3");
    fdt_prop_u32(&f, "#interrupt-cells", 3);
    fdt_prop_empty(&f, "interrupt-controller");
    fdt_prop_u32(&f, "#address-cells", 2);
    fdt_prop_u32(&f, "#size-cells", 2);
    fdt_prop_empty(&f, "ranges");
    fdt_prop_u32(&f, "#redistributor-regions", 1);
    { uint64_t r[] = { WD_GICD_BASE, 0x10000, WD_GICR_BASE, 0x20000 }; fdt_prop_reg64(&f, "reg", r, 2); }
    { uint32_t c[] = { GIC_PPI, 9, IRQ_LEVEL_HI }; fdt_prop_cells(&f, "interrupts", c, 3); }
    fdt_begin(&f, "its@8080000");
    fdt_prop_u32(&f, "phandle", PH_ITS);
    fdt_prop_str(&f, "compatible", "arm,gic-v3-its");
    fdt_prop_empty(&f, "msi-controller");
    fdt_prop_u32(&f, "#msi-cells", 1);
    { uint64_t r[] = { WD_GITS_BASE, 0x20000 }; fdt_prop_reg64(&f, "reg", r, 1); }
    fdt_end(&f);
    fdt_end(&f);

    fdt_begin(&f, "timer");
    { const char *c[] = { "arm,armv8-timer", "arm,armv7-timer", NULL }; fdt_prop_strs(&f, "compatible", c); }
    { uint32_t c[] = { GIC_PPI, 13, IRQ_LEVEL_HI, GIC_PPI, 14, IRQ_LEVEL_HI, GIC_PPI, 11, IRQ_LEVEL_HI, GIC_PPI, 10, IRQ_LEVEL_HI };
      fdt_prop_cells(&f, "interrupts", c, 12); }
    fdt_prop_empty(&f, "always-on");
    fdt_end(&f);

    fdt_begin(&f, "apb-pclk");
    fdt_prop_str(&f, "compatible", "fixed-clock");
    fdt_prop_u32(&f, "#clock-cells", 0);
    fdt_prop_u32(&f, "clock-frequency", 24000000);
    fdt_prop_str(&f, "clock-output-names", "clk24mhz");
    fdt_prop_u32(&f, "phandle", PH_CLK);
    fdt_end(&f);

    fdt_begin(&f, "pl011@9000000");
    { const char *c[] = { "arm,pl011", "arm,primecell", NULL }; fdt_prop_strs(&f, "compatible", c); }
    { uint64_t r[] = { WD_UART_BASE, 0x1000 }; fdt_prop_reg64(&f, "reg", r, 1); }
    { uint32_t c[] = { GIC_SPI, 1, IRQ_LEVEL_HI }; fdt_prop_cells(&f, "interrupts", c, 3); }
    { uint32_t c[] = { PH_CLK, PH_CLK }; fdt_prop_cells(&f, "clocks", c, 2); }
    { const char *c[] = { "uartclk", "apb_pclk", NULL }; fdt_prop_strs(&f, "clock-names", c); }
    fdt_end(&f);

    fdt_begin(&f, "pl031@9010000");
    { const char *c[] = { "arm,pl031", "arm,primecell", NULL }; fdt_prop_strs(&f, "compatible", c); }
    { uint64_t r[] = { WD_RTC_BASE, 0x1000 }; fdt_prop_reg64(&f, "reg", r, 1); }
    { uint32_t c[] = { GIC_SPI, 2, IRQ_LEVEL_HI }; fdt_prop_cells(&f, "interrupts", c, 3); }
    fdt_prop_u32(&f, "clocks", PH_CLK);
    fdt_prop_str(&f, "clock-names", "apb_pclk");
    fdt_end(&f);

    fdt_begin(&f, "flash@0");
    fdt_prop_str(&f, "compatible", "cfi-flash");
    { uint64_t r[] = { 0, WD_FLASH_SIZE, WD_FLASH_SIZE, WD_FLASH_SIZE }; fdt_prop_reg64(&f, "reg", r, 2); }
    fdt_prop_u32(&f, "bank-width", 4);
    fdt_end(&f);

    fdt_begin(&f, "fw-cfg@9020000");
    fdt_prop_str(&f, "compatible", "qemu,fw-cfg-mmio");
    { uint64_t r[] = { WD_FWCFG_BASE, 0x18 }; fdt_prop_reg64(&f, "reg", r, 1); }
    fdt_prop_empty(&f, "dma-coherent");
    fdt_end(&f);

    fdt_begin(&f, "pcie@10000000");
    fdt_prop_str(&f, "compatible", "pci-host-ecam-generic");
    fdt_prop_str(&f, "device_type", "pci");
    fdt_prop_u32(&f, "#address-cells", 3);
    fdt_prop_u32(&f, "#size-cells", 2);
    fdt_prop_u32(&f, "linux,pci-domain", 0);
    { uint32_t c[] = { 0, (uint32_t)(WD_PCIE_ECAM_SZ >> 20) - 1 }; fdt_prop_cells(&f, "bus-range", c, 2); }
    fdt_prop_empty(&f, "dma-coherent");
    { uint64_t r[] = { WD_PCIE_ECAM, WD_PCIE_ECAM_SZ }; fdt_prop_reg64(&f, "reg", r, 1); }
    { uint32_t c[] = { 0, PH_ITS, 0, 0x10000 }; fdt_prop_cells(&f, "msi-map", c, 4); }
    {
        uint32_t c[] = {
            0x01000000, 0, 0, (uint32_t)(WD_PCIE_PIO >> 32), (uint32_t)WD_PCIE_PIO, 0, 0x10000,
            0x02000000, 0, (uint32_t)WD_PCIE_MMIO, 0, (uint32_t)WD_PCIE_MMIO, 0, (uint32_t)WD_PCIE_MMIO_SZ,
            0x03000000, (uint32_t)(WD_PCIE_MMIO_HI >> 32), (uint32_t)WD_PCIE_MMIO_HI,
            (uint32_t)(WD_PCIE_MMIO_HI >> 32), (uint32_t)WD_PCIE_MMIO_HI,
            (uint32_t)(WD_PCIE_MMIO_HI_SZ >> 32), (uint32_t)WD_PCIE_MMIO_HI_SZ,
        };
        fdt_prop_cells(&f, "ranges", c, 21);
    }
    {
        uint32_t map[16 * 10], *p = map;
        for (int slot = 0; slot < 4; slot++)
            for (int pin = 0; pin < 4; pin++) {
                *p++ = (uint32_t)slot << 11; *p++ = 0; *p++ = 0; *p++ = (uint32_t)pin + 1;
                *p++ = PH_GIC; *p++ = 0; *p++ = 0;
                *p++ = GIC_SPI; *p++ = (uint32_t)(3 + ((slot + pin) & 3)); *p++ = IRQ_LEVEL_HI;
            }
        fdt_prop_cells(&f, "interrupt-map", map, 160);
        uint32_t mask[] = { 0x1800, 0, 0, 7 };
        fdt_prop_cells(&f, "interrupt-map-mask", mask, 4);
        fdt_prop_u32(&f, "#interrupt-cells", 1);
    }
    fdt_end(&f);

    fdt_end(&f);                                    /* root */
    free(m->dtb);
    m->dtb = fdt_finish(&f, &m->dtb_size);
    fdt_free(&f);
}

int wd_load_firmware(wd_machine *m, const uint8_t *code, size_t code_len, const uint8_t *vars, size_t vars_len)
{
    if (!code || code_len == 0 || code_len > WD_FLASH_SIZE) {
        snprintf(m->err, sizeof m->err, "firmware image must be 1..64 MiB");
        return -1;
    }
    if (vars_len > WD_FLASH_SIZE) {
        snprintf(m->err, sizeof m->err, "variable store larger than 64 MiB");
        return -1;
    }
    if (m->ram_size < (128ULL << 20)) {
        snprintf(m->err, sizeof m->err, "UEFI needs at least 128 MiB of RAM");
        return -1;
    }
    wd_flash_free(&m->flash[0]);
    wd_flash_free(&m->flash[1]);
    if (wd_flash_init(&m->flash[0], WD_FLASH_SIZE, code, code_len, 1) ||
        wd_flash_init(&m->flash[1], WD_FLASH_SIZE, vars, vars_len, 0)) {
        snprintf(m->err, sizeof m->err, "out of memory for flash");
        return -1;
    }
    if (!wd_fwcfg_file(&m->fwcfg, "etc/ramfb")) wd_ramfb_init(m);
    if (!wd_fwcfg_file(&m->fwcfg, "etc/table-loader")) wd_acpi_build(m);
    build_dtb(m);
    if (m->dtb_size > m->ram_size) return -1;
    memcpy(m->ram, m->dtb, m->dtb_size);           /* ArmVirtQemu reads the tree at the start of RAM */
    m->firmware = 1;
    wd_cpu_reset(m);
    m->cpu.pc = 0;
    m->cpu.x[0] = WD_RAM_BASE;
    wd_tlb_flush(m);
    return 0;
}

const uint8_t *wd_firmware_vars(wd_machine *m, size_t *len, int *dirty)
{
    if (!m->firmware) return NULL;
    *len = (size_t)m->flash[1].used;
    if (dirty) *dirty = m->flash[1].dirty;
    return m->flash[1].mem;
}

void wd_firmware_vars_saved(wd_machine *m)
{
    m->flash[1].dirty = 0;
}

const uint8_t *wd_get_dtb(wd_machine *m, size_t *len)
{
    *len = m->dtb_size;
    return m->dtb;
}

void wd_platform_free(wd_machine *m)
{
    wd_flash_free(&m->flash[0]);
    wd_flash_free(&m->flash[1]);
    wd_fwcfg_free(&m->fwcfg);
    wd_its_free(m);
    for (int i = 0; i < m->npci; i++) {
        if (m->pci[i]->on_free) m->pci[i]->on_free(m->pci[i]);
        free(m->pci[i]->msix_table);
        free(m->pci[i]);
    }
    m->npci = 0;
    free(m->dtb);
    m->dtb = NULL;
}

const uint8_t *wd_fwcfg_blob(wd_machine *m, const char *name, size_t *len)
{
    wd_fwcfg_item *it = wd_fwcfg_file(&m->fwcfg, name);
    if (!it) return NULL;
    *len = it->len;
    return it->data;
}
