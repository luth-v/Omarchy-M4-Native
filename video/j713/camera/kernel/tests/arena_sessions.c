// SPDX-License-Identifier: GPL-2.0-only
/* Three driver sessions of the 17:12 allocation sequence through the driver's
 * own extent allocator, against Stage D's bump layout. Sessions 2 and 3 model
 * boot 1 on the Air: START repeats only 4 of its 5 persistent allocations
 * (+4 entries, +786432 bytes each), which reproduces every logged total.
 * Which 16 KiB persistent allocation is not repeated is unknown, so each
 * candidate runs. Pass --report for the per-allocation table.
 * restart_sessions() then models fw_restart=1: every session is a fresh
 * firmware boot on a reset arena.
 */
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

#define ALLOCATIONS 84
#define BOOT_ALLOCATIONS 12
#define SESSIONS 3
#define FREES_BEFORE_CH_STOP_REPLY 41
/* Arena bases: Stage D at 17:12 and the driver on boot 1. Both are 16 KiB
 * past a 2 MiB boundary and agree modulo 32 MiB, so IOVA alignments up to
 * 16 MiB compare directly.
 */
#define STAGE_D_ARENA 0x10096a04000ULL
#define DRIVER_ARENA 0x10098a04000ULL
#define MIB 0x100000ULL
#define ARRAY_LENGTH(a) (sizeof(a) / sizeof((a)[0]))

struct layout {
	size_t offset[SESSIONS + 1][ALLOCATIONS];
	bool taken[SESSIONS + 1][ALLOCATIONS];
	struct cam_arena_stats stop[SESSIONS + 1];
	size_t high[SESSIONS + 1];
};

static const unsigned int skip_candidates[] = { 18, 33, 41 };

static bool is_persistent(unsigned int index)
{
	unsigned int i;

	for (i = 0; i < 5; i++)
		if (replay_persistent[i] == index)
			return true;
	return false;
}

static u64 lowbit(u64 value)
{
	return value & -value;
}

static size_t bump_offset(unsigned int index)
{
	size_t offset = 0;
	unsigned int i;

	for (i = 0; i < index; i++)
		offset += CAM_ALIGN16K(replay_sizes[i]);
	return offset;
}

static void reclaim(struct cam_arena *arena)
{
	size_t cursor = 0, offset, size;

	cam_arena_begin_reclaim(arena);
	while (cam_arena_reclaim_range(arena, cursor, &offset, &size))
		cursor = offset + size;
	cam_arena_finish_reclaim(arena);
}

static void run(unsigned int skip, struct layout *l)
{
	struct cam_extent *entries = calloc(CAM_ARENA_ENTRIES, sizeof(*entries));
	struct cam_arena arena = {
		.entries = entries, .capacity = CAM_ARENA_ENTRIES, .limit = CAM_ARENA_SIZE,
	};
	unsigned int i, session;

	assert(entries);
	memset(l, 0, sizeof(*l));
	for (i = 0; i < BOOT_ALLOCATIONS; i++) {
		assert(cam_arena_take(&arena, replay_sizes[i], i, &l->offset[1][i]));
		for (session = 1; session <= SESSIONS; session++) {
			l->offset[session][i] = l->offset[1][i];
			l->taken[session][i] = true;
		}
	}
	for (session = 1; session <= SESSIONS; session++) {
		for (i = BOOT_ALLOCATIONS; i < ALLOCATIONS; i++) {
			if (session > 1 && i == skip)
				continue;
			assert(cam_arena_take(&arena, replay_sizes[i], i, &l->offset[session][i]));
			l->taken[session][i] = true;
			if (l->offset[session][i] + CAM_ALIGN16K(replay_sizes[i]) > l->high[session])
				l->high[session] = l->offset[session][i] + CAM_ALIGN16K(replay_sizes[i]);
		}
		if (session == SESSIONS) {
			/* Session 3 went DEAD after its CH_START allocations. */
			l->stop[session] = cam_arena_stats(&arena);
			break;
		}
		for (i = 0; i < ARRAY_LENGTH(replay_frees); i++)
			assert(cam_arena_release(&arena, l->offset[session][replay_frees[i]], 0, 0));
		reclaim(&arena);
		l->stop[session] = cam_arena_stats(&arena);
	}
	free(entries);
}

/* Each session boots the firmware on a reset arena: its 12 boot allocations,
 * START and CH_START, then the 67 frees and the stop's reclaim. The reset
 * discards the 17 extents the previous instance still held at its stop.
 */
static void restart_sessions(unsigned int sessions)
{
	struct cam_extent *entries = calloc(CAM_ARENA_ENTRIES, sizeof(*entries));
	struct cam_arena arena = {
		.entries = entries, .capacity = CAM_ARENA_ENTRIES, .limit = CAM_ARENA_SIZE,
	};
	size_t offset[ALLOCATIONS];
	struct cam_arena_stats stats = { 0 };
	unsigned int i, session;

	assert(entries);
	for (session = 1; session <= sessions; session++) {
		cam_arena_reset(&arena);
		stats = cam_arena_stats(&arena);
		assert(!stats.live && !stats.pending && !stats.entries && !arena.high);
		for (i = 0; i < ALLOCATIONS; i++) {
			assert(cam_arena_take(&arena, replay_sizes[i], i, &offset[i]));
			assert(offset[i] == bump_offset(i));
		}
		assert(arena.high == 277757952);
		for (i = 0; i < ARRAY_LENGTH(replay_frees); i++)
			assert(cam_arena_release(&arena, offset[replay_frees[i]], 0, 0));
		reclaim(&arena);
		stats = cam_arena_stats(&arena);
		/* Session 1's stop on boot 1, every time. */
		assert(stats.live == 18923520 && stats.entries == 17 && !stats.pending &&
		       stats.peak == 277757952 && arena.high == 277757952);
	}
	printf("restart sessions=%u: every session equals Stage D's bump layout, stop live=%zu entries=%u peak=%zu high=%zu\n",
	       sessions, stats.live, stats.entries, stats.peak, arena.high);
	free(entries);
}

static const char *moved(const struct layout *l, unsigned int i)
{
	if (!l->taken[3][i])
		return "not taken";
	if (l->offset[3][i] == l->offset[1][i] && l->offset[3][i] == l->offset[2][i])
		return "";
	if (l->offset[3][i] == l->offset[2][i])
		return "as s2";
	if (l->offset[3][i] == l->offset[1][i])
		return "as s1";
	return "new";
}

static void report(unsigned int skip, const struct layout *l)
{
	unsigned int i, s;

	printf("skip=%u: idx size bump s1 s2 s3 | IOVA align KiB: stageD s1 s2 s3 | s3\n", skip);
	for (i = BOOT_ALLOCATIONS; i < ALLOCATIONS; i++) {
		printf("%2u %#9x %#9zx", i, replay_sizes[i], bump_offset(i));
		for (s = 1; s <= SESSIONS; s++) {
			if (l->taken[s][i])
				printf(" %#9zx", l->offset[s][i]);
			else
				printf(" %9s", "-");
		}
		printf(" | %5llu", (unsigned long long)lowbit(STAGE_D_ARENA + bump_offset(i)) / 1024);
		for (s = 1; s <= SESSIONS; s++)
			printf(" %5llu", l->taken[s][i] ?
			       (unsigned long long)lowbit(DRIVER_ARENA + l->offset[s][i]) / 1024 : 0ULL);
		printf(" | %s%s\n", moved(l, i), is_persistent(i) ? " persistent" : "");
	}
}

static u64 alignment(const struct layout *l, unsigned int session, unsigned int i, bool iova)
{
	if (!session)
		return lowbit((iova ? STAGE_D_ARENA : 0) + bump_offset(i));
	return lowbit((iova ? DRIVER_ARENA : 0) + l->offset[session][i]);
}

/* Allocations of at least 1 MiB that had a grain's alignment in the reference
 * layout (0 is Stage D's bump, else a driver session) and lose it.
 */
static unsigned int alignment_losses(const struct layout *l, unsigned int session,
				     unsigned int reference, bool iova, u64 grain, bool print)
{
	unsigned int i, losses = 0;

	for (i = BOOT_ALLOCATIONS; i < ALLOCATIONS; i++) {
		if (replay_sizes[i] < MIB || !l->taken[session][i] ||
		    (reference && !l->taken[reference][i]))
			continue;
		if (alignment(l, reference, i, iova) >= grain &&
		    alignment(l, session, i, iova) < grain) {
			losses++;
			if (print)
				printf("s%u lost %s %lluM alignment vs %s%u: idx=%u size=%#x\n",
				       session, iova ? "IOVA" : "offset",
				       (unsigned long long)(grain / MIB), reference ? "s" : "Stage D ",
				       reference, i, replay_sizes[i]);
		}
	}
	return losses;
}

static void summary(unsigned int skip, const struct layout *l, unsigned int session, bool print)
{
	unsigned int i, previous = 0, moved = 0, inversions = 0, split = 0;
	u64 smallest = ~0ULL;

	for (i = BOOT_ALLOCATIONS; i < ALLOCATIONS; i++) {
		if (!l->taken[session][i])
			continue;
		if (l->taken[session - 1][i] && l->offset[session][i] != l->offset[session - 1][i])
			moved++;
		if (previous && l->offset[session][i] < l->offset[session][previous])
			inversions++;
		/* Same-size neighbours, such as the frame buffers, were contiguous. */
		if (previous == i - 1 && replay_sizes[i] == replay_sizes[previous] &&
		    l->offset[session][i] != l->offset[session][previous] +
		    CAM_ALIGN16K(replay_sizes[previous]))
			split++;
		if (replay_sizes[i] >= MIB && alignment(l, session, i, true) < smallest)
			smallest = alignment(l, session, i, true);
		previous = i;
	}
	printf("skip=%u s%u moved=%u inversions=%u split_pairs=%u top=%#zx large_min_iova_align=%lluK\n",
	       skip, session, moved, inversions, split, l->high[session],
	       (unsigned long long)smallest / 1024);
	/* Stage D and session 1 share offsets and agree on IOVA alignment to 16 MiB. */
	for (i = 0; i < session; i++)
		printf("skip=%u s%u vs %s%u lost: IOVA 1M/2M=%u/%u offset 1M/2M=%u/%u\n",
		       skip, session, i ? "s" : "stageD", i,
		       alignment_losses(l, session, i, true, MIB, print),
		       alignment_losses(l, session, i, true, 2 * MIB, print),
		       alignment_losses(l, session, i, false, MIB, print),
		       alignment_losses(l, session, i, false, 2 * MIB, print));
}

int main(int argc, char **argv)
{
	bool print = argc > 1 && !strcmp(argv[1], "--report");
	static struct layout l;
	unsigned int c, i, s;
	u64 smallest = ~0ULL;

	for (i = BOOT_ALLOCATIONS; i < ALLOCATIONS; i++)
		if (replay_sizes[i] >= MIB && alignment(&l, 0, i, true) < smallest)
			smallest = alignment(&l, 0, i, true);
	printf("stage_d large_min_iova_align=%lluK\n", (unsigned long long)smallest / 1024);
	assert(smallest == 0x4000);
	for (c = 0; c < ARRAY_LENGTH(skip_candidates); c++) {
		unsigned int skip = skip_candidates[c];

		assert(is_persistent(skip) && replay_sizes[skip] == 0x4000);
		run(skip, &l);
		/* The totals logged on the first hardware boot. */
		assert(l.stop[1].live == 18923520 && l.stop[1].entries == 17 &&
		       l.stop[1].peak == 277757952 && !l.stop[1].pending);
		assert(l.stop[2].live == 19709952 && l.stop[2].entries == 21 &&
		       l.stop[2].peak == 278544384 && !l.stop[2].pending);
		assert(l.stop[3].live == 279330816 && l.stop[3].entries == 92 &&
		       l.stop[3].peak == 279330816 && !l.stop[3].pending);
		/* With nothing freed yet, first fit is Stage D's bump layout. */
		for (i = 0; i < ALLOCATIONS; i++)
			assert(l.offset[1][i] == bump_offset(i));
		assert(l.high[1] == 277757952);
		if (print)
			report(skip, &l);
		for (s = 2; s <= SESSIONS; s++) {
			summary(skip, &l, s, print);
			/* No large buffer loses a 1 or 2 MiB IOVA alignment the proven
			 * layouts gave it. Session 2's chance alignments are reported only.
			 */
			for (i = 0; i < 2; i++) {
				assert(!alignment_losses(&l, s, i, true, MIB, false));
				assert(!alignment_losses(&l, s, i, true, 2 * MIB, false));
			}
		}
	}
	restart_sessions(200);
	puts("arena session replay passed");
	return 0;
}
