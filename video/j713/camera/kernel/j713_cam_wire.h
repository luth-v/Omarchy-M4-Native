/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J713_CAM_WIRE_H
#define J713_CAM_WIRE_H

/* Measured Stage D endpoint-mode layout. */
#define CAM_IPC_BASE 0x10003000000ULL
#define CAM_IPC_SIZE 0x20000UL
#define CAM_IPC_QUEUE_SIZE 0x9840UL
#define CAM_ARGS_OFFSET (CAM_IPC_QUEUE_SIZE + 0x40)
#define CAM_ARGS_IOVA (CAM_IPC_BASE + CAM_ARGS_OFFSET)
#define CAM_EXTRA_BASE 0x10004000000ULL
#define CAM_EXTRA_SIZE 0x2a00000UL
#define CAM_BOOT_MAP_END 0x6a00000UL
#define CAM_PLATFORM_ID 0x17
#define CAM_CHANNELS 7
#define CAM_SLOT_SIZE 0x28
#define CAM_QUERY_RECORD (CAM_IPC_BASE + 0x18000)
#define CAM_QUERY_COMMAND (CAM_IPC_BASE + 0x18400)
#define CAM_QUERY_CAPACITY 0x800
/* GPL apple-isp (H13-H17) never declares less than this for a buffer list;
 * the H17 notes say the firmware reads at least this much of a batch.
 */
#define CAM_BUFEXC_MIN_SIZE 0x280UL
#define CAM_CONFIG_COPY (CAM_IPC_BASE + 0x18c00)
#define CAM_CONFIG_EXT_COPY (CAM_IPC_BASE + 0x19000)
#define CAM_SENSOR_COPY (CAM_IPC_BASE + 0x19600)
#define CAM_CURRENT_COPY (CAM_IPC_BASE + 0x19800)
#define CAM_PRESET_COPY (CAM_IPC_BASE + 0x1a000)
#define CAM_MAX_PRESETS 16
#define CAM_PRESET_STRIDE 0x200
/* Stage D uses the extended M4 configuration size. */
#define CAM_CAMERA_CONFIG_SIZE 0x120
#define CAM_CAPTURE_SIZE 0x800000UL
#define CAM_SETFILE_MAX 0x10000UL
#define CAM_SETFILE_SIZE 0x6ff4UL
#define CAM_LENSCAL_RECORD 0x6fbcUL
#define CAM_LENSCAL_MESSAGE 0x6fc0UL
#define CAM_LENSCAL_SIZE 0x34UL
#define CAM_SETFILE_NAME_SIZE 64
#define CAM_ARENA_SIZE 0x20000000UL
#define CAM_CHANNEL_MAP_END 0x28800000UL
#define CAM_META_COUNT 16
#define CAM_META_STRIDE 0x8000UL
#define CAM_BUFFER_COMMAND 0x30000000ULL
#define CAM_BUFFER_ACK 0x80000000ULL
#define CAM_BUFFER_RENDER 0x10000000ULL
#define CAM_CAPMETA_OFFSET (CAM_META_COUNT * CAM_META_STRIDE)
#define CAM_CAPMETA_STRIDE 0x2c000UL
#define CAM_RENDER_OFFSET (CAM_CAPMETA_OFFSET + CAM_META_COUNT * CAM_CAPMETA_STRIDE)
#define CAM_FRAME_COUNT 2
#define CAM_FRAME_WIDTH 1280U
#define CAM_DELIVER_HEIGHT 720U
#define CAM_DELIVER_Y (CAM_FRAME_WIDTH * CAM_DELIVER_HEIGHT)
#define CAM_DELIVER_SIZE (CAM_DELIVER_Y * 3 / 2)
#define CAM_ALIGN16K(value) (((value) + 0x3fffUL) & ~0x3fffUL)
#define CAM_Y_SPAN(height) CAM_ALIGN16K(CAM_FRAME_WIDTH * (height))
#define CAM_UV_SPAN(height) CAM_ALIGN16K(CAM_FRAME_WIDTH * (height) / 2)
#define CAM_POOL_END(height) (CAM_RENDER_OFFSET + CAM_FRAME_COUNT * \
			     (CAM_Y_SPAN(height) + CAM_UV_SPAN(height)))
#define CAM_ARENA_ENTRIES 1024U
#define CAM_STREAM_SETTLE_MS 10U
#define CAM_STREAM_META_LOW 4
#define CAM_PRINT_BASE 0x10003800000ULL
#define CAM_PRINT_SIZE 0x100000UL
#define CAM_PRINT_RECORD_SIZE 0x90U
#define CAM_PRINT_END (CAM_PRINT_SIZE / CAM_PRINT_RECORD_SIZE * CAM_PRINT_RECORD_SIZE)
#define CAM_DEAD_PRINT_RECORDS 64U

/* Use one locked return snapshot; an unpublished ACK blocks later images. */
static inline unsigned int cam_ready_slots(const u8 *order, unsigned int order_count,
					 u32 completed, u32 acked, u32 *delivered, u8 *ready)
{
	unsigned int i, count = 0;

	for (i = 0; i < order_count; i++) {
		u32 bit = 1U << order[i];

		if (!(completed & bit) || (*delivered & bit))
			continue;
		if (!(acked & bit))
			break;
		ready[count++] = order[i];
		*delivered |= bit;
	}
	return count;
}

struct cam_print_record {
	__le64 timestamp;
	__le16 kind, sequence, available, dropped;
	char text[0x80];
};

static inline bool cam_print_cursor_valid(u32 cursor)
{
	return cursor >= CAM_PRINT_RECORD_SIZE && cursor < CAM_PRINT_END &&
		!(cursor % CAM_PRINT_RECORD_SIZE);
}

static inline u32 cam_print_next(u32 cursor)
{
	return cursor + CAM_PRINT_RECORD_SIZE < CAM_PRINT_END ? cursor + CAM_PRINT_RECORD_SIZE :
		CAM_PRINT_RECORD_SIZE;
}

/* The inverse of cam_print_next for a valid cursor; slot 0 holds the cursors. */
static inline u32 cam_print_prev(u32 cursor)
{
	return cursor > CAM_PRINT_RECORD_SIZE ? cursor - CAM_PRINT_RECORD_SIZE :
		CAM_PRINT_END - CAM_PRINT_RECORD_SIZE;
}

struct cam_buffer {
	__le64 iova[4];
	__le32 flags[4];
	__le32 planes, type, tag, reserved;
};

struct cam_buffer_list {
	__le64 type, count;
	struct cam_buffer buffers[];
};

static inline bool cam_pool_index(u64 base, size_t stride, unsigned int count,
				u64 address, unsigned int *index)
{
	if (!stride || !count || stride > ~0ULL / count || base > ~0ULL - count * stride ||
	    address < base || address - base >= count * stride || (address - base) % stride)
		return false;
	*index = (address - base) / stride;
	return true;
}

static inline bool cam_metadata_index(u64 base, u64 address, unsigned int *index)
{
	return cam_pool_index(base, CAM_META_STRIDE, CAM_META_COUNT, address, index);
}

struct cam_bootargs {
	__le64 reserved_00;
	__le64 ipc_iova;
	__le64 shared_base;
	__le64 shared_size;
	__le64 extra_iova;
	__le64 extra_size;
	__le32 platform_id;
	__le32 reserved_34;
	__le64 log_iova;
	__le64 log_size;
	__le64 log_entry_size;
	__le64 ipc_size;
	__le64 reserved_58[2];
	__le32 descriptor_words;
	__le32 reserved_6c;
	u8 descriptor[0x200];
	u8 reserved_270[0x20];
};

struct cam_channel {
	char name[0x40];
	__le32 type;
	__le32 src;
	__le64 count;
	__le64 iova;
	u8 reserved_58[0xa8];
};

struct cam_channel_expected {
	const char *name;
	u32 type, src, count, offset;
};

struct cam_record {
	__le64 reserved_00;
	__le64 arg[3];
	__le64 reserved_20;
};

struct cam_environment {
	__le32 reserved_00;
	__le16 command;
	__le16 status;
	__le32 print;
	__le32 output_size;
	__le64 handle;
	char name[0x20];
	__le32 component[2];
};

static const struct cam_channel_expected cam_channel_expected[CAM_CHANNELS] = {
	{ "TERMINAL",    2, 0x110, 0x300, 0x0700 },
	{ "IO",          0, 0x121,     8, 0x7f00 },
	{ "DEBUG",       0, 0x141,     8, 0x8040 },
	/* Type 0 measured in the live table at 20:21, correcting the report. */
	{ "BUF_H2T",     0, 0x152,  0x40, 0x8180 },
	{ "BUF_T2H",     1, 0x163,  0x40, 0x8b80 },
	{ "SHAREDMALLOC", 1, 0x173,     8, 0x9580 },
	{ "IO_T2H",      1, 0x133,     8, 0x96c0 },
};

/* Firmware descriptions may retain a low address in the same 4-GiB window. */
static inline u64 cam_ipc_iova(u64 address)
{
	return address <= 0xffffffffULL ? (CAM_IPC_BASE & ~0xffffffffULL) | address : address;
}

/* Only the exact host-owned pools may be returned; rendered planes have
 * independent 16 KiB-aligned spans inside each fixed-size reservation.
 */
struct cam_geometry {
	u32 width, height, y_size, uv_size;
	size_t y_span, uv_span, frame_stride, pool_end;
	u32 crop_y, crop_uv;
};

static inline bool cam_geometry_init(struct cam_geometry *g, u32 height)
{
	if (height != 720 && height != 1264)
		return false;
	*g = (struct cam_geometry) {
		.width = CAM_FRAME_WIDTH, .height = height,
		.y_size = CAM_FRAME_WIDTH * height,
		.uv_size = CAM_FRAME_WIDTH * height / 2,
		.y_span = CAM_Y_SPAN(height), .uv_span = CAM_UV_SPAN(height),
		.frame_stride = CAM_Y_SPAN(height) + CAM_UV_SPAN(height),
		.pool_end = CAM_POOL_END(height),
		.crop_y = (height - CAM_DELIVER_HEIGHT) / 2,
		.crop_uv = (height - CAM_DELIVER_HEIGHT) / 4,
	};
	return true;
}

static inline bool cam_buffer_index(const struct cam_geometry *g, u64 base,
				    const struct cam_buffer *buffer, unsigned int *index)
{
	u64 address = cam_ipc_iova(le64_to_cpu(buffer->iova[0]));
	u32 planes = le32_to_cpu(buffer->planes);

	if (base > ~0ULL - CAM_CAPTURE_SIZE)
		return false;
	switch (le32_to_cpu(buffer->type)) {
	case 0:
		return planes == 1 && cam_metadata_index(base, address, index);
	case 2:
		return planes == 1 && cam_pool_index(base + CAM_CAPMETA_OFFSET, CAM_CAPMETA_STRIDE,
						   CAM_META_COUNT, address, index);
	case 1:
		return planes == 2 && cam_pool_index(base + CAM_RENDER_OFFSET, g->frame_stride,
						   CAM_FRAME_COUNT, address, index) &&
			cam_ipc_iova(le64_to_cpu(buffer->iova[1])) == address + g->y_span;
	default:
		return false;
	}
}

static inline bool cam_channel_valid(const struct cam_channel *channel, unsigned int index)
{
	const struct cam_channel_expected *expected;
	size_t name_size;

	if (index >= CAM_CHANNELS)
		return false;
	expected = &cam_channel_expected[index];
	name_size = strlen(expected->name) + 1;
	return !memcmp(channel->name, expected->name, name_size) &&
		le32_to_cpu(channel->type) == expected->type &&
		le32_to_cpu(channel->src) == expected->src &&
		le64_to_cpu(channel->count) == expected->count &&
		cam_ipc_iova(le64_to_cpu(channel->iova)) == CAM_IPC_BASE + expected->offset;
}

struct cam_returned {
	u32 metadata, capture, images;
	u8 order[CAM_FRAME_COUNT];
	unsigned int count, total;
};

static inline bool cam_validate_returns(const struct cam_geometry *g, u64 base,
				       const struct cam_buffer_list *list, size_t size,
				       bool metadata_issued, bool capture_issued,
				       const struct cam_returned *old,
				       struct cam_returned *result)
{
	struct cam_returned batch = { 0 };
	struct cam_buffer buffer;
	u64 count;
	unsigned int i, index, type;
	u32 *mask;

	if (!metadata_issued || size < sizeof(*list))
		return false;
	memcpy(&count, (const u8 *)list + sizeof(__le64), sizeof(count));
	count = le64_to_cpu(count);
	if (count > 2 * CAM_META_COUNT + CAM_FRAME_COUNT ||
	    count > (size - sizeof(*list)) / sizeof(buffer))
		return false;
	for (i = 0; i < count; i++) {
		memcpy(&buffer, (const u8 *)list + sizeof(*list) + i * sizeof(buffer),
		       sizeof(buffer));
		type = le32_to_cpu(buffer.type);
		if ((type && !capture_issued) || !cam_buffer_index(g, base, &buffer, &index))
			return false;
		mask = type == 0 ? &batch.metadata : type == 2 ? &batch.capture : &batch.images;
		if (*mask & (1U << index))
			return false;
		*mask |= 1U << index;
		if (type == 1)
			batch.order[batch.count++] = index;
	}
	if ((old->metadata & batch.metadata) || (old->capture & batch.capture) ||
	    (old->images & batch.images))
		return false;
	batch.total = count;
	*result = batch;
	return true;
}

enum cam_extent_state {
	CAM_EXTENT_LIVE,
	CAM_EXTENT_PENDING,
	CAM_EXTENT_RECLAIMING,
};

struct cam_extent {
	u64 offset, size, tag;
	enum cam_extent_state state;
};

/* high is the end of the highest extent handed out since the last reset. */
struct cam_arena {
	struct cam_extent *entries;
	unsigned int count, capacity;
	size_t limit, peak, high;
};

struct cam_arena_stats {
	size_t live, pending, peak;
	unsigned int entries;
};

static inline struct cam_arena_stats cam_arena_stats(const struct cam_arena *arena)
{
	struct cam_arena_stats stats = { .entries = arena->count, .peak = arena->peak };
	unsigned int i;

	for (i = 0; i < arena->count; i++) {
		if (arena->entries[i].state == CAM_EXTENT_LIVE)
			stats.live += arena->entries[i].size;
		else
			stats.pending += arena->entries[i].size;
	}
	return stats;
}

/* Pending and reclaiming extents occupy gaps until zeroing has finished. */
static inline bool cam_arena_take(struct cam_arena *arena, u64 size, u64 tag, size_t *offset)
{
	struct cam_arena_stats stats;
	size_t candidate = 0, aligned;
	unsigned int i;

	if (!arena->entries || !arena->limit || arena->limit > CAM_ARENA_SIZE ||
	    (arena->limit & 0x3fff) || !size || size > arena->limit ||
	    arena->count >= arena->capacity)
		return false;
	aligned = CAM_ALIGN16K(size);
	for (i = 0; i < arena->count; i++) {
		if (arena->entries[i].offset - candidate >= aligned)
			break;
		candidate = arena->entries[i].offset + arena->entries[i].size;
	}
	if (aligned > arena->limit - candidate)
		return false;
	memmove(&arena->entries[i + 1], &arena->entries[i],
		(arena->count - i) * sizeof(*arena->entries));
	arena->entries[i] = (struct cam_extent) {
		.offset = candidate, .size = aligned, .tag = tag, .state = CAM_EXTENT_LIVE,
	};
	arena->count++;
	stats = cam_arena_stats(arena);
	if (stats.live + stats.pending > arena->peak)
		arena->peak = stats.live + stats.pending;
	if (candidate + aligned > arena->high)
		arena->high = candidate + aligned;
	*offset = candidate;
	return true;
}

static inline bool cam_arena_release(struct cam_arena *arena, u64 offset, u64 size, u64 tag)
{
	unsigned int i;

	if (size || tag)
		return false;
	for (i = 0; i < arena->count; i++) {
		struct cam_extent *entry = &arena->entries[i];

		if (entry->offset != offset)
			continue;
		if (entry->state != CAM_EXTENT_LIVE)
			return false;
		entry->state = CAM_EXTENT_PENDING;
		return true;
	}
	return false;
}

static inline void cam_arena_begin_reclaim(struct cam_arena *arena)
{
	unsigned int i;

	for (i = 0; i < arena->count; i++)
		if (arena->entries[i].state == CAM_EXTENT_PENDING)
			arena->entries[i].state = CAM_EXTENT_RECLAIMING;
}

/* A cursor by address survives receive-side insertions into the sorted table. */
static inline bool cam_arena_reclaim_range(const struct cam_arena *arena, size_t cursor,
					  size_t *offset, size_t *size)
{
	unsigned int i;

	for (i = 0; i < arena->count; i++) {
		const struct cam_extent *entry = &arena->entries[i];

		if (entry->offset >= cursor && entry->state == CAM_EXTENT_RECLAIMING) {
			*offset = entry->offset;
			*size = entry->size;
			return true;
		}
	}
	return false;
}

static inline void cam_arena_finish_reclaim(struct cam_arena *arena)
{
	unsigned int i, count = 0;

	for (i = 0; i < arena->count; i++)
		if (arena->entries[i].state != CAM_EXTENT_RECLAIMING)
			arena->entries[count++] = arena->entries[i];
	arena->count = count;
}

/* A fresh firmware instance owns nothing: every extent goes, whatever its state. */
static inline void cam_arena_reset(struct cam_arena *arena)
{
	arena->count = 0;
	arena->peak = 0;
	arena->high = 0;
}

struct cam_white_balance {
	int u, v;
	bool seeded;
};

static inline int cam_clamp(int value, int low, int high)
{
	return value < low ? low : value > high ? high : value;
}

static inline void cam_wb_lut(u8 *lut, int offset)
{
	unsigned int i;

	offset = cam_clamp(offset, -48, 48);
	for (i = 0; i < 256; i++)
		lut[i] = cam_clamp((int)i + offset, 0, 255);
}

static inline void cam_crop_copy(const struct cam_geometry *g, u8 *dest, const u8 *source)
{
	memcpy(dest, source + g->crop_y * g->width, CAM_DELIVER_Y);
	memcpy(dest + CAM_DELIVER_Y, source + g->y_span + g->crop_uv * g->width,
	       CAM_DELIVER_Y / 2);
}

/* Sample the cached crop before applying the correction. Means use Q8. */
static inline void cam_white_balance(struct cam_white_balance *wb, u8 *frame)
{
	u8 lut_u[256], lut_v[256];
	u8 *uv = frame + CAM_DELIVER_Y;
	u32 sum_u = 0, sum_v = 0, count = 0;
	unsigned int row, column, i;
	int u, v;

	for (row = 4; row < CAM_DELIVER_HEIGHT; row += 8) {
		for (column = 4; column < CAM_FRAME_WIDTH; column += 8) {
			i = row / 2 * CAM_FRAME_WIDTH + column;
			sum_u += uv[i];
			sum_v += uv[i + 1];
			count++;
		}
	}
	u = sum_u * 256 / count;
	v = sum_v * 256 / count;
	if (!wb->seeded) {
		wb->u = u;
		wb->v = v;
		wb->seeded = true;
	} else {
		wb->u += (u - wb->u) / 8;
		wb->v += (v - wb->v) / 8;
	}
	cam_wb_lut(lut_u, 128 - wb->u / 256);
	cam_wb_lut(lut_v, 128 - wb->v / 256);
	for (i = 0; i < CAM_DELIVER_Y / 2; i += 2) {
		uv[i] = lut_u[uv[i]];
		uv[i + 1] = lut_v[uv[i + 1]];
	}
}

_Static_assert(sizeof(struct cam_extent) == 32, "extent entry");
_Static_assert(sizeof(struct cam_bootargs) == 0x290, "boot header");
_Static_assert(offsetof(struct cam_bootargs, descriptor_words) == 0x68, "descriptor words");
_Static_assert(offsetof(struct cam_bootargs, descriptor) == 0x70, "descriptor");
_Static_assert(offsetof(struct cam_bootargs, ipc_iova) == 8, "IPC pointer");
_Static_assert(offsetof(struct cam_bootargs, log_iova) == 0x38, "log pointer");
_Static_assert(offsetof(struct cam_bootargs, ipc_size) == 0x50, "IPC size");
_Static_assert(sizeof(struct cam_channel) == 0x100, "channel");
_Static_assert(offsetof(struct cam_channel, iova) == 0x50, "channel pointer");
_Static_assert(sizeof(struct cam_record) == CAM_SLOT_SIZE, "record");
_Static_assert(sizeof(struct cam_environment) == 0x40, "environment");
_Static_assert(sizeof(struct cam_buffer) == 0x40, "buffer");
_Static_assert(offsetof(struct cam_buffer, planes) == 0x30, "plane count");
_Static_assert(offsetof(struct cam_buffer, type) == 0x34, "pool type");
_Static_assert(sizeof(struct cam_buffer_list) == 0x10, "list");
_Static_assert(sizeof(struct cam_print_record) == CAM_PRINT_RECORD_SIZE, "print");
_Static_assert(CAM_META_COUNT * CAM_META_STRIDE == CAM_CAPMETA_OFFSET, "metadata pool");
_Static_assert(CAM_CAPMETA_OFFSET + CAM_META_COUNT * CAM_CAPMETA_STRIDE ==
	       CAM_RENDER_OFFSET, "capture metadata pool");
_Static_assert(CAM_POOL_END(720) <= CAM_CAPTURE_SIZE, "720 pool");
_Static_assert(CAM_POOL_END(1264) <= CAM_CAPTURE_SIZE, "1264 pool");
/* The blob sits at the 16 KiB-aligned pool end, as cam_setfile_stage places it. */
_Static_assert(CAM_ALIGN16K(CAM_POOL_END(720)) + CAM_SETFILE_MAX <= CAM_CAPTURE_SIZE,
	       "720 setfile");
_Static_assert(CAM_ALIGN16K(CAM_POOL_END(1264)) + CAM_SETFILE_MAX <= CAM_CAPTURE_SIZE,
	       "1264 setfile");
_Static_assert(CAM_SETFILE_SIZE <= CAM_SETFILE_MAX, "setfile reservation");
_Static_assert(CAM_LENSCAL_MESSAGE + CAM_LENSCAL_SIZE == CAM_SETFILE_SIZE, "block 31 end");
_Static_assert(CAM_BUFEXC_MIN_SIZE <= CAM_QUERY_CAPACITY, "minimum list");
_Static_assert(sizeof(struct cam_buffer_list) + (CAM_META_COUNT + CAM_FRAME_COUNT) *
	       sizeof(struct cam_buffer) <= CAM_QUERY_CAPACITY, "capture list");

#endif
