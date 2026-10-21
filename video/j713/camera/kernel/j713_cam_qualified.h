/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J713_CAM_QUALIFIED_H
#define J713_CAM_QUALIFIED_H

/* iBoot ISP code from macOS 26.5.1 / 25F80, validated on one J713.
 * The physical layout in j713_isp_d.h is part of this identity.
 */
static inline bool j713_cam_firmware_qualified(unsigned int crc)
{
	static const unsigned int qualified[] = { 0xaaafbaa6U };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(qualified); i++)
		if (crc == qualified[i])
			return true;
	return false;
}

#endif
