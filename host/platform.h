/* WindDroid - UEFI platform pieces. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WD_PLATFORM_H
#define WD_PLATFORM_H

#include "../core/internal.h"

void wd_platform_free(wd_machine *m);
const uint8_t *wd_get_dtb(wd_machine *m, size_t *len);

#endif
