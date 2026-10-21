// SPDX-License-Identifier: GPL-2.0-only
/* The production snapshot must refuse unknown firmware and release its copy. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#include "../j713_cam_qualified.h"

#define __iomem
#define D_CODE_PHYS 1
#define D_DATA_PHYS 2
#define D_CODE_SIZE 64
#define D_DATA_SIZE 32
#define SZ_16K 16384
#define GFP_KERNEL 0
#define IORESOURCE_SYSTEM_RAM 0
#define IORES_DESC_NONE 0
#define REGION_DISJOINT 0
#define region_intersects(a, b, c, d) 0
#define vmalloc malloc
#define vfree free
#define kmalloc(size, flags) ({ (void)(flags); malloc(size); })
#define kfree free
#define memcpy_fromio memcpy
#define pr_info(...) ((void)0)
#define pr_err(...) ((void)0)
typedef uint32_t u32;
static unsigned char code_data[D_CODE_SIZE], data_data[D_DATA_SIZE];
static const void *j713_isp_d_pristine_data;
static u32 code_crc, j713_isp_d_code_crc, observed;
static unsigned int unmapped;

static void *ioremap_wc(unsigned long address, size_t size)
{
	assert((address == D_CODE_PHYS && size == D_CODE_SIZE) ||
	       (address == D_DATA_PHYS && size == D_DATA_SIZE));
	return address == D_CODE_PHYS ? code_data : data_data;
}

static void iounmap(void *address)
{
	assert(address == code_data || address == data_data);
	unmapped++;
}

static u32 d_crc_io(void *address, size_t size, void *bounce)
{
	assert(address == code_data && size == D_CODE_SIZE && bounce);
	return observed;
}

#include ".build/snapshot_impl.h"

int main(void)
{
	const u32 candidates[] = { 0, 0xaaafbaa7, 0xaaafbaa6 };
	unsigned int i;

	memset(data_data, 0x5a, sizeof(data_data));
	for (i = 0; i < ARRAY_SIZE(candidates); i++) {
		observed = candidates[i];
		unmapped = 0;
		if (j713_cam_firmware_qualified(observed)) {
			assert(!d_snapshot_firmware());
			assert(j713_isp_d_pristine_data);
			assert(!memcmp(j713_isp_d_pristine_data, data_data, D_DATA_SIZE));
			free((void *)j713_isp_d_pristine_data);
			j713_isp_d_pristine_data = NULL;
		} else {
			assert(d_snapshot_firmware() == -ENODEV);
			assert(!j713_isp_d_pristine_data);
		}
		assert(code_crc == observed && unmapped == 2);
	}
	puts("firmware tests passed: only qualified CRC exports a snapshot; mismatches release resources");
	return 0;
}
