/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J713_ISP_D_POWER_H
#define J713_ISP_D_POWER_H

#include <linux/iopoll.h>

#include "j713_isp_d.h"

/*
 * ISP sub-domain power, following the exact kernel's apple_pmgr_ps_set() and
 * the M2/M3 device trees: isp_set0 (M4 ADT ISP_CPU) is force-disable, and
 * isp_set1/2 (ISP_CPU_CORE0/1) are force-disable plus force-reset. The ISP
 * DARTs are in ps_isp_sys on M3, whose ADT has no dart-isp gates, as on M4.
 */
#define D_PS_RESET BIT(31)
#define D_PS_AUTO_ENABLE BIT(28)
#define D_PS_PS_RESET BIT(12)
#define D_PS_DEV_DISABLE BIT(10)
#define D_PS_FLAGS (BIT(9) | BIT(8))

/* iBoot leaves every sub-domain off except +0x4020, which is clock-gated. */
static inline u32 d_power_boot_target(unsigned int off)
{
	return off == 0x4020 ? 4 : 0;
}

static inline u32 d_power_off_bits(unsigned int off)
{
	if (off == 0x4000)
		return D_PS_DEV_DISABLE;
	if (off == 0x4008 || off == 0x4010)
		return D_PS_DEV_DISABLE | D_PS_PS_RESET;
	return 0;
}

static inline int d_ps_set(void __iomem *pmgr, unsigned int off, u32 target)
{
	u32 reg = readl(pmgr + off), pre, cur;
	int ret;

	/* The earlier panic followed RESET assertion; never sequence through it. */
	if (reg & D_PS_RESET)
		return -EBUSY;
	if (target != D_PS_ACTIVE && d_power_off_bits(off)) {
		pre = (reg & ~(D_PS_AUTO_ENABLE | D_PS_FLAGS)) | d_power_off_bits(off);
		writel(pre, pmgr + off);
		ret = readl_poll_timeout(pmgr + off, cur,
					 (cur & (D_PS_DEV_DISABLE | D_PS_PS_RESET)) ==
					 d_power_off_bits(off), 1, 1000);
		if (ret)
			return ret;
	}
	reg &= ~(D_PS_DEV_DISABLE | D_PS_PS_RESET | D_PS_AUTO_ENABLE |
		 D_PS_FLAGS | D_PS_TARGET);
	writel(reg | target, pmgr + off);
	return readl_poll_timeout(pmgr + off, cur,
				  FIELD_GET(D_PS_ACTUAL, cur) == target, 10, 10000);
}

/* True only for the exact boot state, which a Stage D power-down restores. */
static inline bool d_power_at_boot_state(void __iomem *pmgr)
{
	size_t i;
	u32 value;

	for (i = 0; i < ARRAY_SIZE(d_power_order); i++) {
		value = readl(pmgr + d_power_order[i]);
		if (FIELD_GET(D_PS_ACTUAL, value) != d_power_boot_target(d_power_order[i]) ||
		    FIELD_GET(D_PS_TARGET, value) != d_power_boot_target(d_power_order[i]) ||
		    value & (D_PS_RESET | D_PS_PS_RESET | D_PS_DEV_DISABLE))
			return false;
	}
	return true;
}

/* Same order as the loader and M3's power-domains list. */
static inline int d_power_up(void __iomem *pmgr)
{
	size_t i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(d_power_order); i++) {
		ret = d_ps_set(pmgr, d_power_order[i], D_PS_ACTIVE);
		if (ret)
			return ret;
	}
	return 0;
}

/* Reverse order, as apple_isp_power_down_domains(); report the first failure. */
static inline int d_power_down(void __iomem *pmgr)
{
	size_t i = ARRAY_SIZE(d_power_order);
	int ret, first = 0;

	while (i--) {
		ret = d_ps_set(pmgr, d_power_order[i], d_power_boot_target(d_power_order[i]));
		if (ret && !first)
			first = ret;
	}
	return first;
}

#endif
