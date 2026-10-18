// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <endian.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
#define le16_to_cpu le16toh
#define le32_to_cpu le32toh
#define le64_to_cpu le64toh
#include "../j713_cam_wire.h"
#include "replay.h"

static void arena_rules(void)
{
	struct cam_extent entries[4];
	struct cam_arena arena = { .entries = entries, .capacity = 4, .limit = 0x40000 };
	struct cam_arena_stats stats;
	size_t a, b, c, off, size;

	assert(!cam_arena_take(&arena, 0, 0, &off));
	assert(!cam_arena_take(&arena, UINT64_MAX, 0, &off));
	assert(cam_arena_take(&arena, 1, 1, &a) && a == 0);
	assert(cam_arena_take(&arena, 0x4001, 2, &b) && b == 0x4000);
	assert(!cam_arena_release(&arena, a, 1, 0));
	assert(!cam_arena_release(&arena, a, 0, 1));
	assert(!cam_arena_release(&arena, 1, 0, 0));
	assert(!cam_arena_release(&arena, UINT64_MAX, 0, 0));
	assert(cam_arena_release(&arena, a, 0, 0));
	assert(!cam_arena_release(&arena, a, 0, 0));
	assert(cam_arena_take(&arena, 0x4000, 3, &c) && c == 0xc000);
	cam_arena_begin_reclaim(&arena);
	assert(cam_arena_reclaim_range(&arena, 0, &off, &size) && off == a && size == 0x4000);
	assert(!cam_arena_release(&arena, a, 0, 0));
	assert(cam_arena_take(&arena, 0x4000, 4, &off) && off == 0x10000);
	assert(!cam_arena_take(&arena, 0x4000, 5, &off));
	/* A free arriving during zeroing stays pending for the next stop. */
	assert(cam_arena_release(&arena, b, 0, 0));
	cam_arena_finish_reclaim(&arena);
	assert(arena.count == 3);
	assert(cam_arena_take(&arena, 0x4000, 6, &off) && off == a);
	assert(arena.count == 4);
	stats = cam_arena_stats(&arena);
	assert(stats.live == 0xc000 && stats.pending == 0x8000 && stats.peak == 0x14000);
	cam_arena_begin_reclaim(&arena);
	assert(cam_arena_reclaim_range(&arena, 0, &off, &size) && off == b && size == 0x8000);
	assert(!cam_arena_reclaim_range(&arena, off + size, &off, &size));
	cam_arena_finish_reclaim(&arena);
	assert(cam_arena_take(&arena, 0x8000, 7, &off) && off == b);
	arena.capacity = 20;
	arena.count = 0;
	assert(cam_arena_take(&arena, arena.limit, 0, &off) && off == 0);
	assert(!cam_arena_take(&arena, 1, 0, &off));
	assert(cam_arena_release(&arena, 0, 0, 0));
	assert(!cam_arena_take(&arena, 1, 0, &off));
	cam_arena_begin_reclaim(&arena);
	assert(!cam_arena_take(&arena, 1, 0, &off));
	cam_arena_finish_reclaim(&arena);
	assert(cam_arena_take(&arena, arena.limit, 0, &off));
}

/* A firmware restart drops every extent, live, pending or reclaiming. */
static void arena_reset(void)
{
	struct cam_extent entries[4];
	struct cam_arena arena = { .entries = entries, .capacity = 4, .limit = 0x40000 };
	struct cam_arena_stats stats;
	size_t a, b, c, off, size;

	assert(!arena.high);
	assert(cam_arena_take(&arena, 0x4000, 1, &a) && a == 0 && arena.high == 0x4000);
	assert(cam_arena_take(&arena, 0x8000, 2, &b) && b == 0x4000 && arena.high == 0xc000);
	assert(cam_arena_take(&arena, 0x10000, 3, &c) && c == 0xc000 && arena.high == 0x1c000);
	/* Frees, reclaim and refills never lower the zeroing bound. */
	assert(cam_arena_release(&arena, c, 0, 0));
	cam_arena_begin_reclaim(&arena);
	assert(cam_arena_reclaim_range(&arena, 0, &off, &size) && off == c && size == 0x10000);
	cam_arena_finish_reclaim(&arena);
	assert(arena.count == 2 && arena.high == 0x1c000);
	assert(cam_arena_take(&arena, 0x4000, 4, &off) && off == c && arena.high == 0x1c000);
	/* Reset while b is reclaiming and a is pending. */
	assert(cam_arena_release(&arena, b, 0, 0));
	cam_arena_begin_reclaim(&arena);
	assert(cam_arena_release(&arena, a, 0, 0));
	stats = cam_arena_stats(&arena);
	assert(stats.live == 0x4000 && stats.pending == 0xc000 && stats.peak == 0x1c000);
	cam_arena_reset(&arena);
	stats = cam_arena_stats(&arena);
	assert(!stats.live && !stats.pending && !stats.peak && !stats.entries && !arena.high);
	assert(arena.entries == entries && arena.capacity == 4 && arena.limit == 0x40000);
	/* No reclaiming range survives, and allocation restarts at the base. */
	assert(!cam_arena_reclaim_range(&arena, 0, &off, &size));
	assert(!cam_arena_release(&arena, a, 0, 0));
	assert(cam_arena_take(&arena, 0x40000, 5, &off) && off == 0 && arena.high == 0x40000);
	assert(!cam_arena_take(&arena, 1, 6, &off));
	cam_arena_reset(&arena);
	assert(cam_arena_take(&arena, 1, 7, &off) && off == 0 && arena.count == 1);
	assert(arena.peak == 0x4000 && arena.high == 0x4000);
}

static bool is_persistent(unsigned int index)
{
	unsigned int i;

	for (i = 0; i < 5; i++)
		if (replay_persistent[i] == index)
			return true;
	return false;
}

static void arena_replay(bool new_persistent)
{
	struct cam_extent *entries = calloc(CAM_ARENA_ENTRIES, sizeof(*entries));
	struct cam_arena arena = {
		.entries = entries, .capacity = CAM_ARENA_ENTRIES, .limit = CAM_ARENA_SIZE,
	};
	struct cam_arena_stats stats;
	size_t offsets[84], offset, size, cursor, boot = 0, retained = 0;
	unsigned int i, session, stopped = 0;
	bool failed = false;

	assert(entries);
	for (i = 0; i < 12; i++) {
		assert(cam_arena_take(&arena, replay_sizes[i], i, &offsets[i]));
		boot += replay_sizes[i];
	}
	for (i = 0; i < 5; i++)
		retained += replay_sizes[replay_persistent[i]];
	for (session = 1; session <= 200; session++) {
		for (i = 12; i < 84; i++) {
			if (!new_persistent && session > 1 && is_persistent(i))
				continue;
			if (!cam_arena_take(&arena, replay_sizes[i], i, &offsets[i])) {
				failed = true;
				break;
			}
		}
		if (failed)
			break;
		stats = cam_arena_stats(&arena);
		assert(stats.live == boot + retained * (new_persistent ? session : 1) +
		       258834432);
		for (i = 0; i < 67; i++)
			assert(cam_arena_release(&arena, offsets[replay_frees[i]], 0, 0));
		stats = cam_arena_stats(&arena);
		assert(stats.live == boot + retained * (new_persistent ? session : 1));
		assert(stats.pending == 258834432);
		cam_arena_begin_reclaim(&arena);
		cursor = 0;
		while (cam_arena_reclaim_range(&arena, cursor, &offset, &size))
			cursor = offset + size;
		cam_arena_finish_reclaim(&arena);
		stats = cam_arena_stats(&arena);
		assert(!stats.pending && stats.entries == 12 + 5 * (new_persistent ? session : 1));
		stopped++;
		if (session <= 3)
			printf("replay new=%d session=%u live=%zu entries=%u\n",
			       new_persistent, session, stats.live, stats.entries);
	}
	assert(new_persistent ? (failed && session == 190 && stopped == 189 && arena.count == 1024) :
	       (!failed && stopped == 200));
	printf("replay new=%d completed=%u table_failure_session=%u peak=%zu\n",
	       new_persistent, stopped, failed ? session : 0, arena.peak);
	free(entries);
}

static void geometry_and_returns(unsigned int height)
{
	struct cam_geometry g;
	struct cam_returned old = { 0 }, batch;
	struct cam_buffer_list *list = calloc(1, CAM_QUERY_CAPACITY);
	u64 base = 0x100c0000000ULL;
	unsigned int type, index, slot;
	u32 cursor;

	assert(list && cam_geometry_init(&g, height));
	assert(g.width == 1280 && g.height == height);
	assert(g.y_size <= g.y_span && g.uv_size <= g.uv_span);
	assert(!(g.y_span % 0x4000) && !(g.uv_span % 0x4000));
	assert(g.pool_end <= CAM_CAPTURE_SIZE && CAM_RENDER_OFFSET == 0x340000);
	assert(g.crop_y == (height == 720 ? 0 : 272));
	assert(g.crop_uv == g.crop_y / 2);
	assert(g.y_span == (height == 720 ? 0xe4000 : 0x18c000));
	assert(g.uv_span == (height == 720 ? 0x74000 : 0xc8000));
	assert(g.pool_end == (height == 720 ? 0x5f0000 : 0x7e8000));
	for (type = 0; type < 3; type++) {
		for (slot = 0; slot < (type == 1 ? CAM_FRAME_COUNT : CAM_META_COUNT); slot++) {
			struct cam_buffer *b = &list->buffers[0];
			u64 address = base + (type == 1 ? CAM_RENDER_OFFSET + slot * g.frame_stride :
					     type == 2 ? CAM_CAPMETA_OFFSET + slot * CAM_CAPMETA_STRIDE :
					     slot * CAM_META_STRIDE);

			memset(list, 0, CAM_QUERY_CAPACITY);
			list->count = htole64(1);
			b->type = htole32(type);
			b->planes = htole32(type == 1 ? 2 : 1);
			b->iova[0] = htole64(address);
			b->iova[1] = htole64(address + g.y_span);
			assert(cam_buffer_index(&g, base, b, &index) && index == slot);
			assert(cam_validate_returns(&g, base, list, 0x50, true, true, &old, &batch));
			assert(!cam_validate_returns(&g, base, list, 0x4f, true, true, &old, &batch));
			assert(!cam_validate_returns(&g, base, list, 0x50, false, true, &old, &batch));
			if (type)
				assert(!cam_validate_returns(&g, base, list, 0x50, true, false, &old, &batch));
			assert(!cam_validate_returns(&g, base, list, 0x50, true, true, &batch, &batch));
			b->iova[0] = htole64(address & 0xffffffffULL);
			b->iova[1] = htole64((address + g.y_span) & 0xffffffffULL);
			assert(cam_buffer_index(&g, base, b, &index));
			b->iova[0] = htole64(address + 1);
			assert(!cam_buffer_index(&g, base, b, &index));
			b->iova[0] = htole64(address);
			b->planes = htole32(type == 1 ? 1 : 2);
			assert(!cam_buffer_index(&g, base, b, &index));
			b->planes = htole32(type == 1 ? 2 : 1);
			if (type == 1) {
				b->iova[1] = htole64(address + g.y_span + 1);
				assert(!cam_buffer_index(&g, base, b, &index));
				b->iova[1] = htole64(address + g.y_span);
			}
			assert(!cam_buffer_index(&g, UINT64_MAX, b, &index));
			list->buffers[1] = *b;
			list->count = htole64(2);
			assert(!cam_validate_returns(&g, base, list, 0x90, true, true, &old, &batch));
			b->type = htole32(8);
			assert(!cam_buffer_index(&g, base, b, &index));
		}
	}
	{
		u8 bytes[sizeof(struct cam_buffer_list) + 4] = { 0 };

		assert(cam_validate_returns(&g, base, (const void *)(bytes + 4),
					    sizeof(struct cam_buffer_list), true, false, &old, &batch));
		assert(batch.total == 0);
	}
	list->count = htole64(UINT64_MAX);
	assert(!cam_validate_returns(&g, base, list, CAM_QUERY_CAPACITY, true, true, &old, &batch));
	assert(!cam_pool_index(0, 0, 2, 0, &index));
	assert(!cam_pool_index(0, SIZE_MAX, 16, 0, &index));
	for (index = 0; index < CAM_CHANNELS; index++) {
		const struct cam_channel_expected *e = &cam_channel_expected[index];
		struct cam_channel channel = { 0 };

		memcpy(channel.name, e->name, strlen(e->name) + 1);
		channel.type = htole32(e->type);
		channel.src = htole32(e->src);
		channel.count = htole64(e->count);
		channel.iova = htole64(CAM_IPC_BASE + e->offset);
		assert(cam_channel_valid(&channel, index));
		channel.count = htole64(UINT64_MAX);
		assert(!cam_channel_valid(&channel, index));
	}
	assert(!cam_print_cursor_valid(0) && !cam_print_cursor_valid(CAM_PRINT_END));
	cursor = CAM_PRINT_RECORD_SIZE;
	for (index = 0; index < CAM_PRINT_END / CAM_PRINT_RECORD_SIZE - 1; index++) {
		assert(cam_print_cursor_valid(cursor));
		assert(cam_print_prev(cam_print_next(cursor)) == cursor);
		assert(cam_print_cursor_valid(cam_print_prev(cursor)));
		cursor = cam_print_next(cursor);
	}
	assert(cursor == CAM_PRINT_RECORD_SIZE);
	assert(cam_print_prev(CAM_PRINT_RECORD_SIZE) == CAM_PRINT_END - CAM_PRINT_RECORD_SIZE);
	/* The DEAD dump steps back from the producer and stays inside the ring. */
	for (index = 0; index < CAM_DEAD_PRINT_RECORDS; index++)
		cursor = cam_print_prev(cursor);
	assert(cursor == CAM_PRINT_END - CAM_DEAD_PRINT_RECORDS * CAM_PRINT_RECORD_SIZE);
	assert(cursor + CAM_PRINT_RECORD_SIZE * CAM_DEAD_PRINT_RECORDS <= CAM_PRINT_SIZE);
	free(list);
}

static void crop_and_white_balance(unsigned int height)
{
	struct cam_geometry g;
	struct cam_white_balance wb = { 0 };
	u8 lut[256];
	u8 *source, *dest;
	unsigned int row, column, i;

	assert(cam_geometry_init(&g, height));
	source = malloc(g.frame_stride);
	dest = malloc(CAM_DELIVER_SIZE);
	assert(source && dest);
	memset(source, 0xa5, g.frame_stride);
	for (row = 0; row < height; row++)
		memset(source + row * g.width, row % 251, g.width);
	for (row = 0; row < height / 2; row++) {
		for (column = 0; column < g.width; column += 2) {
			source[g.y_span + row * g.width + column] = row % 251;
			source[g.y_span + row * g.width + column + 1] = (row + 31) % 251;
		}
	}
	cam_crop_copy(&g, dest, source);
	for (row = 0; row < CAM_DELIVER_HEIGHT; row++)
		for (column = 0; column < g.width; column++)
			assert(dest[row * g.width + column] == (row + g.crop_y) % 251);
	assert(!memcmp(dest + CAM_DELIVER_Y, source + g.y_span + g.crop_uv * g.width,
		       CAM_DELIVER_Y / 2));
	for (row = 0; row < CAM_DELIVER_HEIGHT / 2; row++) {
		for (column = 0; column < g.width; column += 2) {
			assert(dest[CAM_DELIVER_Y + row * g.width + column] == (row + g.crop_uv) % 251);
			assert(dest[CAM_DELIVER_Y + row * g.width + column + 1] ==
			       (row + g.crop_uv + 31) % 251);
		}
	}
	for (i = 0; i < g.uv_size; i += 2) {
		source[g.y_span + i] = 143;
		source[g.y_span + i + 1] = 114;
	}
	cam_crop_copy(&g, dest, source);
	cam_white_balance(&wb, dest);
	assert(wb.seeded && wb.u == 143 * 256 && wb.v == 114 * 256);
	for (i = CAM_DELIVER_Y; i < CAM_DELIVER_SIZE; i++)
		assert(dest[i] == 128);
	for (i = 0; i < CAM_DELIVER_Y / 2; i += 2) {
		dest[CAM_DELIVER_Y + i] = 135;
		dest[CAM_DELIVER_Y + i + 1] = 122;
	}
	cam_white_balance(&wb, dest);
	assert(wb.u == 142 * 256 && wb.v == 115 * 256);
	cam_wb_lut(lut, -999);
	assert(lut[0] == 0 && lut[47] == 0 && lut[255] == 207);
	cam_wb_lut(lut, 999);
	assert(lut[0] == 48 && lut[208] == 255 && lut[255] == 255);
	free(source);
	free(dest);
}

int main(void)
{
	struct cam_geometry g;

	assert(!cam_geometry_init(&g, 1260));
	assert(CAM_ARGS_IOVA == 0x10003009880ULL);
	assert(CAM_ARGS_OFFSET + sizeof(struct cam_bootargs) <= CAM_IPC_SIZE);
	assert(CAM_QUERY_COMMAND + CAM_QUERY_CAPACITY <= CAM_CONFIG_COPY);
	arena_rules();
	arena_reset();
	arena_replay(false);
	arena_replay(true);
	geometry_and_returns(720);
	geometry_and_returns(1264);
	crop_and_white_balance(720);
	crop_and_white_balance(1264);
	puts("wire tests passed");
	return 0;
}
