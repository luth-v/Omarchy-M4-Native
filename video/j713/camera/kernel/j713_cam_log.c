// SPDX-License-Identifier: GPL-2.0-only
#include "j713_cam.h"

static void cam_dead_print_dump(struct cam_device *cam)
{
	void *ring = cam_pointer(cam, CAM_PRINT_BASE, CAM_PRINT_SIZE);
	struct cam_print_record record;
	u32 producer, cursor;
	unsigned int i;

	lockdep_assert_held(&cam->fw_lock);
	if (!cam->firmware_started || !ring)
		return;
	producer = le32_to_cpu(READ_ONCE(*(__le32 *)ring));
	if (!cam_print_cursor_valid(producer)) {
		dev_info(cam->dev, "DEAD: no firmware print-ring dump, producer=%#x\n", producer);
		return;
	}
	dma_rmb();
	for (cursor = producer, i = 0; i < CAM_DEAD_PRINT_RECORDS; i++)
		cursor = cam_print_prev(cursor);
	dev_info(cam->dev, "DEAD: firmware print ring, %u slots before producer=%#x host=%#x\n",
		 CAM_DEAD_PRINT_RECORDS, producer, cam->print_cursor);
	for (i = CAM_DEAD_PRINT_RECORDS; i; i--, cursor = cam_print_next(cursor)) {
		memcpy(&record, ring + cursor, sizeof(record));
		if (!le16_to_cpu(record.kind) && record.text[0])
			dev_info(cam->dev, "ISP[-%u]: %.*s\n", i, (int)sizeof(record.text), record.text);
		else if (le16_to_cpu(record.kind))
			cam_print_other(cam, &record, i);
	}
}

void cam_dead_dump(struct cam_device *cam)
{
	lockdep_assert_held(&cam->fw_lock);
	if (!cam_dead(cam) || cam->dead_dumped)
		return;
	cam->dead_dumped = true;
	cam_dead_print_dump(cam);
}
