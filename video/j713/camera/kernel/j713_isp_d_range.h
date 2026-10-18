/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J713_ISP_D_RANGE_H
#define J713_ISP_D_RANGE_H

/* Subtraction avoids wrapping the exclusive end of a firmware request. */
static inline bool d_range_offset(u64 addr, u64 base, size_t length,
				  size_t size, size_t *offset)
{
	u64 delta;

	if (!size || addr < base)
		return false;
	delta = addr - base;
	if (delta > length || size > length - delta)
		return false;
	*offset = delta;
	return true;
}

#endif
