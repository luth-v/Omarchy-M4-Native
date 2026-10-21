/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J713_ISP_D_MMIO_H
#define J713_ISP_D_MMIO_H

/* ISP DAPF entries from this Air's ADT, rounded to 16-KiB pages. These are
 * firmware DART translations only; no AP register access is performed.
 */
struct d_mmio_window {
	u64 base;
	size_t size;
};

static const struct d_mmio_window d_mmio_windows[] = {
	{ 0x220004000, 0x14000 },
	{ 0x220044000, 0x14000 },
	{ 0x2201c4000, 0x14000 },
	{ 0x222004000, 0x14000 },
	{ 0x222044000, 0x14000 },
	{ 0x2221c4000, 0x14000 },
	{ 0x3803c0000, 0x28000 },
	{ 0x380704000, 0x4000 },
	{ 0x380730000, 0x4000 },
	{ 0x380e3c000, 0x4000 },
	{ 0x382824000, 0x4000 },
	{ 0x39062c000, 0x20000 },
	{ 0x501660000, 0x4000 },
};

#endif
