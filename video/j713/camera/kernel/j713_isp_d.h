/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J713_ISP_D_H
#define J713_ISP_D_H

#include <linux/bitfield.h>
#include <linux/crc32.h>
#include <linux/io.h>
#include <linux/minmax.h>
#include <linux/of.h>
#include <linux/sizes.h>
#include <linux/utsname.h>

#include "j713_cam_qualified.h"

#define D_KERNEL "7.1.9-m4-gpu-backport-20261008"
#define D_PMGR 0x380700000ULL
#define D_PMGR_SIZE 0x14000
#define D_ISP_SYS 0x530
#define D_PS_ACTUAL GENMASK(7, 4)
#define D_PS_TARGET GENMASK(3, 0)
#define D_PS_ACTIVE 0xf
#define D_NODE "/soc/isp-stage-d@48d600000"

/* iBoot-preloaded firmware, outside Linux System RAM. */
#define D_CODE_PHYS 0x10000d20000ULL
#define D_CODE_SIZE 0xca4000UL
#define D_DATA_PHYS 0x10001f30000ULL
#define D_DATA_SIZE 0x748000UL

/*
 * ISP CPU external debug block. The M1-M3 apple-isp driver writes EDPRCR at
 * ISP +0x1010310 before each boot; RVBAR kept its M1-M3 offset (+0x1050000)
 * on M4. Only debug-power-domain registers are accessed.
 */
#define D_DEBUG 0x48d010000ULL
#define D_DEBUG_SIZE 0x1000
#define D_EDPRCR 0x310
#define D_EDPRCR_CWRR BIT(1)
#define D_EDPRSR 0x314
#define D_EDLSR 0xfb4
#define D_EDAUTHSTATUS 0xfb8
#define D_EDDEVARCH 0xfbc
#define D_EDDEVTYPE 0xfcc
#define D_EDPIDR4 0xfd0
#define D_EDPIDR0 0xfe0
#define D_EDCIDR0 0xff0
#define D_CORESIGHT_CIDR 0xb105900dU

/* Kbuild embeds this DTBO in the per-boot loader. */
extern const u8 __dtbo_j713_isp_stage_d_begin[];
extern const u8 __dtbo_j713_isp_stage_d_end[];

/* Captured by the loader before any firmware start in this power-on. */
extern const void *j713_isp_d_pristine_data;
extern u32 j713_isp_d_code_crc;

static const unsigned int d_power_order[] = {
	0x4000, 0x4008, 0x4010, 0x4018, 0x4020, 0x4030, 0x4038,
	0x4040, 0x4048, 0x4028, 0x4050, 0x4058, 0x4060,
};

static inline bool d_machine_ok(void)
{
	return of_machine_is_compatible("apple,j713") &&
	       of_machine_is_compatible("apple,t8132") &&
	       !strcmp(utsname()->release, D_KERNEL);
}

static inline bool d_power_active(void __iomem *pmgr, unsigned int off)
{
	u32 value = readl(pmgr + off);

	/* Active power alone does not make MMIO safe while reset is asserted. */
	return FIELD_GET(D_PS_ACTUAL, value) == D_PS_ACTIVE &&
	       !(value & (BIT(31) | BIT(12) | BIT(10)));
}

/* CRC of a write-combined firmware mapping, through a SZ_16K bounce buffer. */
static inline u32 d_crc_io(const void __iomem *src, size_t size, void *bounce)
{
	u32 crc = ~0U;
	size_t off, len;

	for (off = 0; off < size; off += len) {
		len = min_t(size_t, size - off, SZ_16K);
		memcpy_fromio(bounce, src + off, len);
		crc = crc32_le(crc, bounce, len);
	}
	return ~crc;
}

#endif
