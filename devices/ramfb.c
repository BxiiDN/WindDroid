/*
 * WindDroid - ramfb: a framebuffer in guest RAM configured through the
 * fw_cfg file "etc/ramfb" (as QEMU's hw/display/ramfb.c). EDK2's
 * QemuRamfbDxe turns it into a UEFI Graphics Output Protocol, and Windows
 * keeps drawing into the same memory through its basic display driver.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "../core/internal.h"

#define FOURCC_XR24 0x34325258u   /* XRGB8888 */
#define FOURCC_AR24 0x34325241u   /* ARGB8888 */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void on_write(wd_machine *m, wd_fwcfg_item *it)
{
    const uint8_t *c = it->data;
    uint64_t addr = (uint64_t)be32(c) << 32 | be32(c + 4);
    uint32_t fourcc = be32(c + 8), width = be32(c + 16), height = be32(c + 20), stride = be32(c + 24);
    if (fourcc != FOURCC_XR24 && fourcc != FOURCC_AR24) return;
    if (width < 16 || height < 16 || width > 16384 || height > 16384) return;
    if (!stride) stride = width * 4;
    if (stride < width * 4) return;
    uint64_t size = (uint64_t)stride * (height - 1) + (uint64_t)width * 4;
    if (!wd_phys_is_ram(m, addr) || !wd_phys_is_ram(m, addr + size - 1)) return;
    m->fb.addr = addr;
    m->fb.width = width;
    m->fb.height = height;
    m->fb.stride = stride;
    m->fb.valid = 1;
    m->fb.generation++;
}

void wd_ramfb_init(wd_machine *m)
{
    wd_fwcfg_item *it = wd_fwcfg_add_file(&m->fwcfg, "etc/ramfb", NULL, 28, 1);
    if (it) it->on_write = on_write;
}

int wd_framebuffer(wd_machine *m, wd_fb_info *info)
{
    if (!m->fb.valid) return 0;
    info->pixels = m->ram + (m->fb.addr - WD_RAM_BASE);
    info->width = m->fb.width;
    info->height = m->fb.height;
    info->stride = m->fb.stride;
    info->generation = m->fb.generation;
    return 1;
}
