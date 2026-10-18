// SPDX-License-Identifier: GPL-2.0-only
#include "j713_cam.h"

static u8 *cam_command(struct cam_device *cam, u16 opcode)
{
	u8 *command;

	if (!cam_can_send(cam))
		return NULL;
	command = cam_pointer(cam, CAM_QUERY_COMMAND, CAM_QUERY_CAPACITY);
	if (!command) {
		cam_fail(cam, "unmapped command buffer");
		return NULL;
	}
	memset(command, 0, CAM_QUERY_CAPACITY);
	put_unaligned_le16(opcode, command + 4);
	return command;
}

/* cam_message logs every command, its reply and its elapsed time. */
static int cam_io(struct cam_device *cam, size_t insize, size_t outsize)
{
	u8 *command = cam_pointer(cam, CAM_QUERY_COMMAND, CAM_QUERY_CAPACITY);
	u16 opcode = get_unaligned_le16(command + 4);
	int ret = cam_exchange(cam, 0x21, insize, outsize);
	u16 status = get_unaligned_le16(command + 6);
	char reason[64];

	if (!ret && status) {
		scnprintf(reason, sizeof(reason), "IPC command %#x returned status %#x", opcode, status);
		cam_fail(cam, reason);
		cam_dead_dump(cam);
		ret = -EPROTO;
	}
	return ret;
}

/* Only experiment helpers use this status policy. */
static int cam_experiment_exchange(struct cam_device *cam, size_t insize, size_t outsize,
				   int *result)
{
	u8 *command = cam_pointer(cam, CAM_QUERY_COMMAND, CAM_QUERY_CAPACITY);
	u16 opcode = get_unaligned_le16(command + 4);
	u16 status;
	int ret;

	WRITE_ONCE(*result, -EINPROGRESS);
	ret = cam_exchange(cam, 0x21, insize, outsize);
	status = get_unaligned_le16(command + 6);
	if (!ret && status) {
		dev_info(cam->dev,
			 "experiment command %#x rejected: status=%#x words=%08x %08x %08x %08x\n",
			 opcode, status, get_unaligned_le32(command + 8),
			 get_unaligned_le32(command + 0xc), get_unaligned_le32(command + 0x10),
			 get_unaligned_le32(command + 0x14));
		ret = -EPROTO;
	}
	WRITE_ONCE(*result, ret);
	return ret;
}

static int cam_experiment_io(struct cam_device *cam, size_t size, int *result)
{
	return cam_experiment_exchange(cam, size, size, result);
}

/* Frame-rate requests: channel 0, rational bounds for 0x260, Q8 for the pair. */
static int cam_fps_set_rate(struct cam_device *cam, u16 opcode, unsigned int rate)
{
	u8 *command;
	int ret;

	if (cam_dead(cam))
		return -EIO;
	command = cam_command(cam, opcode);
	if (!command)
		return -EIO;
	put_unaligned_le32(0, command + 8);
	if (opcode == 0x260) {
		command[0xc] = 0;
		put_unaligned_le32(rate, command + 0x10);
		put_unaligned_le32(1, command + 0x14);
		put_unaligned_le32(rate, command + 0x18);
		put_unaligned_le32(1, command + 0x1c);
		return cam_experiment_exchange(cam, 0x20, 0x20, &ret);
	}
	put_unaligned_le32(rate << 8, command + 0xc);
	return cam_experiment_exchange(cam, 0x10, 0x10, &ret);
}

static int cam_fps_get_rate(struct cam_device *cam, u16 opcode, u32 *rate)
{
	u8 *command;
	int ret;

	if (cam_dead(cam))
		return -EIO;
	command = cam_command(cam, opcode);
	if (!command)
		return -EIO;
	put_unaligned_le32(0, command + 8);
	ret = cam_experiment_exchange(cam, 0xc, 0x10, &ret);
	if (!ret)
		*rate = get_unaligned_le32(command + 0xc);
	return ret;
}

/* MAX_GET, then MIN_GET. A rejected GET reads as 0; a timeout or DEAD is fatal. */
static int cam_fps_range(struct cam_device *cam, u32 *maximum, int *max_ret,
			 u32 *minimum, int *min_ret)
{
	*maximum = 0;
	*minimum = 0;
	*max_ret = cam_fps_get_rate(cam, 0x207, maximum);
	if (cam_dead(cam) || (*max_ret && *max_ret != -EPROTO))
		return *max_ret ?: -EIO;
	*min_ret = cam_fps_get_rate(cam, 0x209, minimum);
	if (cam_dead(cam) || (*min_ret && *min_ret != -EPROTO))
		return *min_ret ?: -EIO;
	return 0;
}

/* SetMinMaxFrameRate asserts min <= max after each SET of the pair. A rate at
 * or above the current maximum raises the maximum first. A lower rate, or a
 * range that reads as 0, moves the minimum first: the firmware's inferred AE
 * maximum of 60 keeps that safe for both rates.
 */
static int cam_fps_pair(struct cam_device *cam, unsigned int rate)
{
	u32 minimum, maximum;
	int ret, min_ret, max_ret;
	bool max_first;

	ret = cam_fps_range(cam, &maximum, &max_ret, &minimum, &min_ret);
	if (ret)
		return ret;
	max_first = minimum && maximum && rate << 8 >= maximum;
	dev_info(cam->dev, "fps: pair order %s: max_q8=%u max_result=%d min_q8=%u min_result=%d\n",
		 max_first ? "max-first" : "min-first", maximum, max_ret, minimum, min_ret);
	ret = cam_fps_set_rate(cam, max_first ? 0x208 : 0x20a, rate);
	if (!ret && !cam_dead(cam))
		ret = cam_fps_set_rate(cam, max_first ? 0x20a : 0x208, rate);
	return ret;
}

/* S3.5: after CH_START and any S3 setfile load, before the capture list. */
int cam_fps_start(struct cam_device *cam)
{
	unsigned int rate = cam->session.experiment.frame_rate;
	u32 minimum, maximum;
	int ret, min_ret, max_ret, read;

	lockdep_assert_held(&cam->fw_lock);
	if (!rate || cam_dead(cam))
		return 0;
	ret = cam_fps_set_rate(cam, 0x260, rate);
	if (ret == -EPROTO && !cam_dead(cam)) {
		dev_info(cam->dev, "fps: 0x260 rejected, trying 0x208/0x20a pair\n");
		ret = cam_fps_pair(cam, rate);
	}
	if (cam_dead(cam) || (ret && ret != -EPROTO))
		return ret ?: -EIO;
	if (ret)
		dev_info(cam->dev, "fps: pin rejected, giving up for session\n");
	read = cam_fps_range(cam, &maximum, &max_ret, &minimum, &min_ret);
	if (read)
		return read;
	dev_info(cam->dev, "fps: requested=%u pin_result=%d min_q8=%u min_result=%d max_q8=%u max_result=%d\n",
		 rate, ret, minimum, min_ret, maximum, max_ret);
	return 0;
}

/* GET requests are header-only, zero-padded to the spec's reply-sized I/O. */
int cam_framing_crop(struct cam_device *cam, const char *when)
{
	u8 *command;
	int ret;

	if (cam_dead(cam))
		return -EIO;
	command = cam_command(cam, 0x800);
	if (!command)
		return -EIO;
	put_unaligned_le32(0, command + 8);
	ret = cam_experiment_io(cam, 0x2c, &ret);
	if (!ret)
		dev_info(cam->dev,
			 "framing %s: cropX=%u cropY=%u cropW=%u cropH=%u boundX=%u boundY=%u boundW=%u boundH=%u\n",
			 when, get_unaligned_le32(command + 0xc), get_unaligned_le32(command + 0x10),
			 get_unaligned_le32(command + 0x14), get_unaligned_le32(command + 0x18),
			 get_unaligned_le32(command + 0x1c), get_unaligned_le32(command + 0x20),
			 get_unaligned_le32(command + 0x24), get_unaligned_le32(command + 0x28));
	return ret;
}

int cam_framing_zoom(struct cam_device *cam, const char *when, u16 zoom[2])
{
	u8 *command;
	int ret;

	if (cam_dead(cam))
		return -EIO;
	command = cam_command(cam, 0x811);
	if (!command)
		return -EIO;
	put_unaligned_le32(0, command + 8);
	ret = cam_experiment_io(cam, 0x10, &ret);
	if (!ret) {
		zoom[0] = get_unaligned_le16(command + 0xc);
		zoom[1] = get_unaligned_le16(command + 0xe);
		/* A periodic poll passes no label; its caller logs only drift. */
		if (when)
			dev_info(cam->dev, "framing %s: zoom scaler0=%#x scaler1=%#x (Q10, 0x400=1x)\n",
				 when, zoom[0], zoom[1]);
	}
	return ret;
}

int cam_framing_pin(struct cam_device *cam)
{
	u8 *command;
	int ret;

	if (cam_dead(cam))
		return -EIO;
	command = cam_command(cam, 0x810);
	if (!command)
		return -EIO;
	put_unaligned_le32(0, command + 8);
	put_unaligned_le16(0x400, command + 0xc);
	return cam_experiment_io(cam, 0x10, &ret);
}

int cam_lenscal_get(struct cam_device *cam)
{
	u8 *command = cam_command(cam, 0x519);
	int ret;

	if (!command)
		return -EIO;
	put_unaligned_le32(0, command + 8);
	ret = cam_experiment_io(cam, CAM_LENSCAL_SIZE, &cam_lenscal_result);
	dev_info(cam->dev,
		 "lens-cal GET: result=%d status=%#x supported=%#x words=%08x %08x %08x tail=%24phN\n",
		 ret, get_unaligned_le16(command + 6), get_unaligned_le32(command + 0xc),
		 get_unaligned_le32(command + 0x10), get_unaligned_le32(command + 0x14),
		 get_unaligned_le32(command + 0x18), command + 0x1c);
	return ret;
}

int cam_lenscal_set(struct cam_device *cam, unsigned int number)
{
	u8 *command = cam_command(cam, 0x50f);
	int ret;

	if (!command)
		return -EIO;
	memcpy(command + 0xc, cam->session.experiment.blob->data + CAM_LENSCAL_MESSAGE + 0xc,
	       CAM_LENSCAL_SIZE - 0xc);
	ret = cam_experiment_io(cam, CAM_LENSCAL_SIZE, &cam_lenscal_result);
	dev_info(cam->dev, "lens-cal SET %u: result=%d status=%#x\n",
		 number, ret, get_unaligned_le16(command + 6));
	return ret;
}

int cam_setfile_stage(struct cam_device *cam)
{
	u64 address = cam->capture_base + CAM_ALIGN16K(cam->geometry.pool_end);
	void *target = cam_pointer(cam, address, CAM_SETFILE_MAX);

	if (!target)
		return -EIO;
	memcpy(target, cam->session.experiment.blob->data, CAM_SETFILE_SIZE);
	/* Match blk0gridzero, changing only the capture-pool copy. */
	if (cam->session.experiment.colour_active)
		memset(target + 0x34, 0, 0x78 - 0x34);
	return 0;
}

int cam_setfile_load(struct cam_device *cam)
{
	u64 address = cam->capture_base + CAM_ALIGN16K(cam->geometry.pool_end);
	u8 *command;
	int ret;

	dma_wmb();
	command = cam_command(cam, 0x111);
	if (!command)
		return -EIO;
	put_unaligned_le64(address, command + 0xc);
	put_unaligned_le32(CAM_SETFILE_SIZE, command + 0x14);
	ret = cam_experiment_io(cam, 0x18, &cam_setfile_result);
	dev_info(cam->dev, "setfile load %s address=%#llx size=%#x result=%d status=%#x\n",
		 "apple/isp_2224_01XX.dat", address, (unsigned int)CAM_SETFILE_SIZE,
		 ret, get_unaligned_le16(command + 6));
	return ret;
}

int cam_sensor_info(struct cam_device *cam)
{
	u8 *command = cam_command(cam, 0x10d);
	int ret;

	if (!command)
		return -EIO;
	ret = cam_io(cam, 0x190, 0x190);
	if (ret)
		return ret;
	cam->sensor_version = get_unaligned_le32(command + 0x20);
	cam->presets = get_unaligned_le32(command + 0x60);
	cam->word_68 = get_unaligned_le32(command + 0x68);
	cam->word_78 = get_unaligned_le32(command + 0x78);
	/* Unprinted fields include the module serial number. */
	if (cam->sensor_version != 0x958 || !cam->presets || cam->presets > CAM_MAX_PRESETS)
		return -EPROTO;
	return 0;
}

int cam_boot_queries(struct cam_device *cam)
{
	struct cam_environment *query = (void *)cam_command(cam, 0x1b);
	u8 *command;
	u32 timestamp, channels;
	int ret;

	if (!query)
		return -EIO;
	query->status = cpu_to_le16(0xffff);
	ret = cam_exchange(cam, 0x23, sizeof(*query), sizeof(*query));
	if (ret)
		return ret;
	if (le16_to_cpu(query->status) || !le64_to_cpu(query->handle))
		return -EPROTO;
	command = cam_command(cam, 3);
	if (!command)
		return -EIO;
	ret = cam_io(cam, 0x1c, 0x1c);
	if (ret)
		return ret;
	timestamp = get_unaligned_le32(command + 8);
	channels = get_unaligned_le32(command + 0xc);
	if (!timestamp || !channels || channels > 4)
		return -EPROTO;
	command = cam_command(cam, 0x19);
	if (!command)
		return -EIO;
	ret = cam_io(cam, 0x420, 0x420);
	if (!ret)
		ret = cam_sensor_info(cam);
	return ret;
}

int cam_platform(struct cam_device *cam)
{
	u8 *command = cam_command(cam, 0x3206);

	if (!command)
		return -EIO;
	command[8] = 2;
	put_unaligned_le32(0x2fc, command + 0xc);
	put_unaligned_le64(0x2201d4000, command + 0x10);
	put_unaligned_le64(0x2221d4000, command + 0x18);
	return cam_io(cam, 0x20, 0);
}

int cam_control(struct cam_device *cam, bool start)
{
	if (!cam_command(cam, start ? 0 : 1))
		return -EIO;
	return cam_io(cam, 0xc, 0);
}

int cam_select(struct cam_device *cam)
{
	const struct {
		u16 opcode, insize, outsize;
		u32 value;
	} steps[] = {
		{ 0x24, 0xc, 0xc, 0 },
		{ 0x13b, 0x10, 0, 1 },
		{ 0x107, 0x10, 0, 0 },
	};
	unsigned int i;
	u8 *command;
	int ret;

	for (i = 0; i < ARRAY_SIZE(steps); i++) {
		command = cam_command(cam, steps[i].opcode);
		if (!command)
			return -EIO;
		put_unaligned_le32(steps[i].value, command + (steps[i].insize == 0xc ? 8 : 0xc));
		ret = cam_io(cam, steps[i].insize, steps[i].outsize);
		if (ret || cam_dead(cam))
			return ret ? ret : -EIO;
	}
	return 0;
}

int cam_current(struct cam_device *cam)
{
	u8 *command = cam_command(cam, 0x105);
	u16 width, height, out_width, out_height;
	int ret;

	if (!command)
		return -EIO;
	ret = cam_io(cam, CAM_CAMERA_CONFIG_SIZE, CAM_CAMERA_CONFIG_SIZE);
	if (ret)
		return ret;
	width = get_unaligned_le16(command + 0x10);
	height = get_unaligned_le16(command + 0x12);
	out_width = get_unaligned_le16(command + 0x14);
	out_height = get_unaligned_le16(command + 0x16);
	if (!width || !height || !out_width || !out_height || width > 8192 ||
	    height > 8192 || out_width > 8192 || out_height > 8192)
		return -EPROTO;
	return 0;
}

int cam_channel_input(struct cam_device *cam, u16 opcode, u32 value, bool argument)
{
	u8 *command = cam_command(cam, opcode);

	if (!command)
		return -EIO;
	if (argument)
		put_unaligned_le32(value, command + 0xc);
	return cam_io(cam, argument ? 0x10 : 0xc, 0);
}

int cam_configure_buffers(struct cam_device *cam)
{
	const struct cam_geometry *g = &cam->geometry;
	const u16 pools[] = { 0, 8, 1 };
	unsigned int i;
	u8 *command;
	u32 size;
	int ret;

	if (!cam->word_68 || cam->word_68 > CAM_META_STRIDE ||
	    !cam->word_78 || cam->word_78 > CAM_CAPMETA_STRIDE)
		return -EPROTO;
	ret = cam_channel_input(cam, 0x10e, 1, true);
	if (!ret)
		ret = cam_channel_input(cam, 0x10f, 0, false);
	if (ret || cam_dead(cam))
		return ret ? ret : -EIO;
	command = cam_command(cam, 0xb01);
	if (!command)
		return -EIO;
	put_unaligned_le32(g->width, command + 0xc);
	put_unaligned_le32(g->height, command + 0x10);
	put_unaligned_le32(1, command + 0x14);
	put_unaligned_le32(g->width, command + 0x1c);
	put_unaligned_le32(g->width, command + 0x20);
	put_unaligned_le32(g->height, command + 0x2c);
	put_unaligned_le32(g->width, command + 0x34);
	ret = cam_io(cam, 0x38, 0);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(pools); i++) {
		command = cam_command(cam, 0x117);
		if (!command || cam_dead(cam))
			return -EIO;
		put_unaligned_le16(pools[i], command + 0xc);
		put_unaligned_le16(16, command + 0xe);
		size = pools[i] == 8 ? cam->word_78 : cam->word_68;
		put_unaligned_le32(pools[i] == 1 ? g->y_size : size, command + 0x10);
		put_unaligned_le32(pools[i] == 1 ? g->width : size, command + 0x14);
		if (pools[i] == 1) {
			put_unaligned_le32(g->uv_size, command + 0x30);
			put_unaligned_le32(g->width, command + 0x34);
		}
		put_unaligned_le16(pools[i] == 1 ? 2 : 1, command + 0x94);
		ret = cam_io(cam, 0x9c, 0x9c);
		if (ret)
			return ret;
	}
	ret = cam_channel_input(cam, 0x125, 1, true);
	if (!ret && !cam_dead(cam))
		ret = cam_channel_input(cam, 0xb0d, 1, true);
	return ret;
}

static void cam_list_buffer(struct cam_device *cam, struct cam_buffer *buffer,
			    unsigned int type, unsigned int index)
{
	const struct cam_geometry *g = &cam->geometry;
	u64 address = cam->capture_base;

	if (type == 1)
		address += CAM_RENDER_OFFSET + index * g->frame_stride;
	else if (type == 2)
		address += CAM_CAPMETA_OFFSET + index * CAM_CAPMETA_STRIDE;
	else
		address += index * CAM_META_STRIDE;
	buffer->iova[0] = cpu_to_le64(address);
	buffer->flags[0] = cpu_to_le32(0x40000000);
	buffer->planes = cpu_to_le32(type == 1 ? 2 : 1);
	buffer->type = cpu_to_le32(type);
	if (type == 1) {
		buffer->iova[1] = cpu_to_le64(address + g->y_span);
		buffer->flags[1] = cpu_to_le32(0x40000000);
	}
}

static struct cam_buffer_list *cam_list(struct cam_device *cam)
{
	struct cam_buffer_list *list;

	if (!cam_can_send(cam))
		return NULL;
	list = cam_pointer(cam, CAM_QUERY_COMMAND, CAM_QUERY_CAPACITY);
	memset(list, 0, CAM_QUERY_CAPACITY);
	list->type = cpu_to_le64(1);
	return list;
}

static int cam_submit(struct cam_device *cam, unsigned int count)
{
	struct cam_record *record = cam_pointer(cam, CAM_QUERY_RECORD, sizeof(*record));
	size_t size = max(sizeof(struct cam_buffer_list) + count * sizeof(struct cam_buffer),
			  CAM_BUFEXC_MIN_SIZE);
	int ret = cam_message(cam, 0x24, size, CAM_BUFFER_COMMAND);

	if (!ret && le64_to_cpu(record->arg[2]) != CAM_BUFFER_ACK) {
		cam_fail(cam, "invalid buffer submission acknowledgement");
		ret = -EPROTO;
	}
	return ret;
}

int cam_metadata(struct cam_device *cam)
{
	struct cam_buffer_list *list = cam_list(cam);
	unsigned long flags;
	unsigned int i;
	int ret;

	if (!list || !cam->bt_iova)
		return -EIO;
	list->count = cpu_to_le64(CAM_META_COUNT);
	for (i = 0; i < CAM_META_COUNT; i++)
		cam_list_buffer(cam, &list->buffers[i], 0, i);
	spin_lock_irqsave(&cam->service_lock, flags);
	cam->session.metadata_issued = true;
	spin_unlock_irqrestore(&cam->service_lock, flags);
	ret = cam_submit(cam, CAM_META_COUNT);
	WRITE_ONCE(cam->session.metadata_submitted, !ret);
	return ret;
}

int cam_capture(struct cam_device *cam)
{
	struct cam_buffer_list *list = cam_list(cam);
	unsigned long flags;
	unsigned int i;
	int ret;

	if (!list)
		return -EIO;
	list->count = cpu_to_le64(CAM_META_COUNT + CAM_FRAME_COUNT);
	for (i = 0; i < CAM_META_COUNT; i++)
		cam_list_buffer(cam, &list->buffers[i], 2, i);
	for (i = 0; i < CAM_FRAME_COUNT; i++)
		cam_list_buffer(cam, &list->buffers[CAM_META_COUNT + i], 1, i);
	spin_lock_irqsave(&cam->service_lock, flags);
	cam->session.capture_issued = true;
	spin_unlock_irqrestore(&cam->service_lock, flags);
	ret = cam_submit(cam, CAM_META_COUNT + CAM_FRAME_COUNT);
	WRITE_ONCE(cam->session.capture_submitted, !ret);
	return ret;
}

int cam_requeue(struct cam_device *cam, u32 meta, u32 images)
{
	struct cam_buffer_list *list = cam_list(cam);
	unsigned long flags;
	unsigned int i, count = 0;
	int ret;

	if (!list)
		return -EIO;
	for (i = 0; i < CAM_META_COUNT; i++)
		if (meta & BIT(i))
			cam_list_buffer(cam, &list->buffers[count++], 0, i);
	for (i = 0; i < CAM_FRAME_COUNT; i++)
		if (images & BIT(i))
			cam_list_buffer(cam, &list->buffers[count++], 1, i);
	list->count = cpu_to_le64(count);
	spin_lock_irqsave(&cam->service_lock, flags);
	cam->session.returned.metadata &= ~meta;
	cam->session.returned.images &= ~images;
	cam->session.acked_images &= ~images;
	cam->session.acked_meta &= ~meta;
	cam->session.completed &= ~images;
	cam->session.order_count = 0;
	reinit_completion(&cam->frames);
	spin_unlock_irqrestore(&cam->service_lock, flags);
	ret = cam_submit(cam, count);
	if (!ret)
		cam->session.requeues++;
	return ret;
}

int cam_poweroff_sensor(struct cam_device *cam)
{
	u8 *command = cam_command(cam, 0x1b);
	int ret;

	if (!command)
		return -EIO;
	put_unaligned_le32(1, command + 8);
	ret = cam_io(cam, 0xc, 0xc);
	if (!ret)
		cam_delay(cam, 250);
	return ret;
}
