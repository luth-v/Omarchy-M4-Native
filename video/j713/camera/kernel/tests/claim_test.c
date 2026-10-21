// SPDX-License-Identifier: GPL-2.0-only
/* The production probe-time claim walk and host window on a synthetic page map.
 * No kernel, no physical memory: alloc_contig_range is a model of busy pages.
 */
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long long u64;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
#define le16_to_cpu le16toh
#define le32_to_cpu le32toh
#define le64_to_cpu le64toh
#define U64_MAX UINT64_MAX
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PAGE_SIZE 0x4000UL
#define SZ_2M 0x200000UL
#define PFN_UP(x) (((x) + PAGE_SIZE - 1) / PAGE_SIZE)
#define PFN_DOWN(x) ((x) / PAGE_SIZE)
#define PFN_PHYS(x) ((u64)(x) * PAGE_SIZE)
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ACR_FLAGS_NONE 0
#define GFP_KERNEL 0
#define dma_wmb() (barriers++)

#include "../j713_cam_wire.h"
#include "../j713_isp_d_range.h"
#include ".build/claim_constants.h"

struct page { int unused; };
struct cam_device {
	void *dev, *identity, *ram;
	struct page *claim_pages;
	unsigned long claim_count;
	size_t capture_span, ram_size;
	u64 window_base, arena_base, capture_base;
};

static struct cam_device cam;
static unsigned long busy[64], calls[4096][2];
static unsigned int busy_count, call_count, barriers, alias_fail;
static void *zeroed;
static size_t zeroed_size;
static char log_line[128];

#define pfn_to_page(pfn) ((struct page *)(uintptr_t)(pfn))

static int alloc_contig_range(unsigned long start, unsigned long end, int flags, int gfp)
{
	unsigned int i;

	assert(!flags && !gfp && start < end && call_count < ARRAY_SIZE(calls));
	calls[call_count][0] = start;
	calls[call_count++][1] = end;
	for (i = 0; i < busy_count; i++)
		if (busy[i] >= start && busy[i] < end)
			return -EBUSY;
	return 0;
}

static void dev_err(void *dev, const char *format, ...)
{
	va_list args;

	(void)dev;
	va_start(args, format);
	vsnprintf(log_line, sizeof(log_line), format, args);
	va_end(args);
}

/* Reserve the alias lazily: the walk only writes the two guard pages. */
static int cam_identity_alias(struct cam_device *device)
{
	void *alias;

	if (alias_fail)
		return -ENOMEM;
	alias = mmap(NULL, device->claim_count * PAGE_SIZE, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	assert(alias != MAP_FAILED);
	device->identity = alias;
	return 0;
}

static void *record_zeroing(void *dest, int value, size_t size)
{
	assert(!value && !zeroed);
	zeroed = dest;
	zeroed_size = size;
	return dest;
}

#define memset record_zeroing
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include ".build/claim_impl.h"
#pragma GCC diagnostic pop
#undef memset

#define STEP (SZ_2M / PAGE_SIZE)
#define SMALL (CAM_IDENTITY_SIZE / PAGE_SIZE + 2)
#define LARGE (CAM_IDENTITY_MAX / PAGE_SIZE + 2)
#define FIRST ALIGN(PFN_UP(CAM_VM_BASE + CAM_CHANNEL_MAP_END), STEP)
#define END PFN_DOWN(CAM_VM_BASE + CAM_SHARED_VA_END)
#define DIV_UP(a, b) (((a) + (b) - 1) / (b))

static bool free_range(unsigned long start, unsigned long end)
{
	unsigned int i;

	for (i = 0; i < busy_count; i++)
		if (busy[i] >= start && busy[i] < end)
			return false;
	return true;
}

/* 82f9fd25: the first 2 MiB step whose original-size claim is free, or 0. */
static unsigned long reference_walk(void)
{
	unsigned long pfn;

	for (pfn = FIRST; pfn + SMALL <= END; pfn += STEP)
		if (free_range(pfn, pfn + SMALL))
			return pfn;
	return 0;
}

static void release(void)
{
	if (cam.identity)
		assert(!munmap(cam.identity, cam.claim_count * PAGE_SIZE));
	cam = (struct cam_device) { 0 };
	zeroed = NULL;
	zeroed_size = 0;
	call_count = 0;
	barriers = 0;
	log_line[0] = 0;
}

/* Walk order: per start, the 1080p size if it fits the bounds, then the original. */
static void check_calls(unsigned long final, bool *large)
{
	unsigned long pfn;
	unsigned int i = 0;

	for (pfn = FIRST; pfn + SMALL <= END; pfn += STEP) {
		if (pfn + LARGE <= END) {
			assert(i < call_count && calls[i][0] == pfn && calls[i][1] == pfn + LARGE);
			i++;
			if (free_range(pfn, pfn + LARGE)) {
				assert(pfn == final && i == call_count);
				*large = true;
				return;
			}
		}
		assert(i < call_count && calls[i][0] == pfn && calls[i][1] == pfn + SMALL);
		i++;
		if (free_range(pfn, pfn + SMALL)) {
			assert(pfn == final && i == call_count);
			*large = false;
			return;
		}
	}
	assert(!final && i == call_count);
}

static void check_claim(void)
{
	unsigned long expected = reference_walk(), pfn, i;
	bool large = false;
	int ret;

	release();
	ret = cam_claim(&cam);
	check_calls(expected, &large);
	if (!expected) {
		assert(ret == -ENOMEM && !cam.claim_pages && !cam.identity && !zeroed);
		assert(!strcmp(log_line, "no free 35970-page identity claim in proven shared-VA bounds\n"));
		return;
	}
	pfn = expected;
	/* The original claim's placement, and the large one wherever it also fits. */
	assert(!ret && !log_line[0]);
	assert(large == (pfn + LARGE <= END && free_range(pfn, pfn + LARGE)));
	assert(cam.claim_pages == pfn_to_page(pfn) && cam.claim_count == (large ? LARGE : SMALL));
	assert(cam.capture_span == (large ? CAM_CAPTURE_MAX : CAM_CAPTURE_SIZE));
	assert(cam.window_base == PFN_PHYS(pfn + 1));
	assert(cam.arena_base == cam.window_base + CAM_EXTRA_SIZE);
	assert(cam.capture_base == cam.arena_base + CAM_ARENA_SIZE);
	assert(zeroed == (u8 *)cam.identity + PAGE_SIZE && barriers == 1);
	assert(zeroed_size == (large ? CAM_IDENTITY_MAX : CAM_IDENTITY_SIZE));
	for (i = 0; i < PAGE_SIZE / sizeof(u64); i++) {
		assert(((u64 *)cam.identity)[i] == CAM_GUARD_PATTERN(0));
		assert(((u64 *)((u8 *)cam.identity + (cam.claim_count - 1) * PAGE_SIZE))[i] ==
		       CAM_GUARD_PATTERN(cam.claim_count - 1));
	}
	/* The host window is exactly the mapped claim, minus both guard pages. */
	assert(cam_pointer(&cam, cam.window_base, 1) == (u8 *)cam.identity + PAGE_SIZE);
	assert(!cam_pointer(&cam, cam.window_base - 1, 1));
	assert(cam_pointer(&cam, cam.capture_base + cam.capture_span - 1, 1));
	assert(!cam_pointer(&cam, cam.capture_base + cam.capture_span, 1));
	assert(!cam_pointer(&cam, cam.capture_base, cam.capture_span + 1));
}

/* Busy pages that block every start in [FIRST, through]: each blocks SMALL starts. */
static void block_until(unsigned long through)
{
	unsigned long pfn;

	busy_count = 0;
	for (pfn = FIRST; pfn < through; pfn += SMALL - 1)
		busy[busy_count++] = pfn;
	busy[busy_count++] = through;
	assert(busy_count < ARRAY_SIZE(busy));
}

int main(void)
{
	unsigned long last_large = FIRST + (END - LARGE - FIRST) / STEP * STEP;
	unsigned long last_small = FIRST + (END - SMALL - FIRST) / STEP * STEP;
	unsigned int trial, i;
	u64 seed = 0x1080;

	/* 82f9fd25's walk: 35970 pages; the 1080p claim adds 368 pages (5.75 MiB). */
	assert(SMALL == 35970 && LARGE == SMALL + 368 && last_small > last_large);
	assert((last_small - FIRST) / STEP > 1000);
	busy_count = 0;
	check_claim();
	assert(call_count == 1 && cam.capture_span == CAM_CAPTURE_MAX &&
	       cam.claim_pages == pfn_to_page(FIRST));
	/* A busy page only in the 1080p tail keeps 82f9fd25's start, without 1080p. */
	busy[0] = FIRST + SMALL;
	busy_count = 1;
	check_claim();
	assert(call_count == 2 && cam.capture_span == CAM_CAPTURE_SIZE &&
	       cam.claim_pages == pfn_to_page(FIRST));
	/* A busy page in the original claim moves both sizes on together. */
	busy[0] = FIRST;
	check_claim();
	assert(call_count == 3 && cam.capture_span == CAM_CAPTURE_MAX &&
	       cam.claim_pages == pfn_to_page(FIRST + STEP));
	busy[0] = FIRST + SMALL - 1;
	check_claim();
	assert(cam.claim_pages == pfn_to_page(FIRST + DIV_UP(SMALL, STEP) * STEP));
	/* Near the end only the original size fits; no attempt leaves the bounds. */
	block_until(last_large);
	check_claim();
	assert(cam.capture_span == CAM_CAPTURE_SIZE &&
	       cam.claim_pages == pfn_to_page(last_large + STEP));
	for (i = 0; i < call_count; i++)
		assert(calls[i][1] <= END);
	/* Nothing free anywhere: 82f9fd25's refusal, count and message. */
	block_until(last_small);
	check_claim();
	assert(call_count == 2 * ((last_large - FIRST) / STEP + 1) + (last_small - last_large) / STEP);
	/* Random busy maps: always 82f9fd25's start; 1080p exactly where it fits. */
	for (trial = 0; trial < 300; trial++) {
		busy_count = 1 + trial % 40;
		for (i = 0; i < busy_count; i++) {
			seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
			busy[i] = FIRST + (seed >> 33) % (END - FIRST);
		}
		check_claim();
	}
	/* A failed alias returns its error after placing the claim. */
	busy_count = 0;
	alias_fail = 1;
	release();
	assert(cam_claim(&cam) == -ENOMEM && cam.claim_pages == pfn_to_page(FIRST) && !zeroed);
	alias_fail = 0;
	release();
	puts("claim tests passed: 82f9fd25 placement and refusal preserved, 1080p tail only where free, walk order and bounds, guards and host window");
	return 0;
}
