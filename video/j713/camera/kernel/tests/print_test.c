// SPDX-License-Identifier: GPL-2.0-only
/* Extracted live poll and DEAD dump run against a synthetic firmware ring. */
#include <assert.h>
#include <endian.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t __le16;
typedef uint32_t u32;
typedef unsigned long long u64;
typedef uint32_t __le32;
typedef uint64_t __le64;
#define le16_to_cpu le16toh
#define le32_to_cpu le32toh
#define le64_to_cpu le64toh
#define cpu_to_le32 htole32
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define lockdep_assert_held(lock) assert(*(lock))
#define scnprintf snprintf
#define HZ 1000U
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#include "../j713_cam_wire.h"

struct completion { unsigned int done; };
struct cam_device {
	void *dev;
	bool fw_lock, print_failed, transport_dead, firmware_started, dead;
	u32 print_cursor, banners;
	struct completion ready;
	struct { struct cam_output_evidence output; } session;
};
struct ratelimit_state {
	unsigned int interval, burst, used;
};
#define DEFINE_RATELIMIT_STATE(name, period, count) \
	struct ratelimit_state name = { .interval = (period), .burst = (count) }
static struct ratelimit_state *limiter;
static struct cam_device cam;
static u8 ring[CAM_PRINT_SIZE];
static char logs[65536], levels[256];
static unsigned int barriers, writes, failures, lines, calls;
static bool cam_fw_log, mapped = true;
#define dma_rmb() (barriers++)
#define dma_mb() (barriers++)
#define dma_wmb() (writes++)

static bool __ratelimit(struct ratelimit_state *state)
{
	assert(state->interval == 60 * HZ && state->burst == 10);
	limiter = state;
	calls++;
	return state->used++ < state->burst;
}

static void log_line(char level, const char *format, va_list args)
{
	size_t used = strlen(logs);
	int length = vsnprintf(logs + used, sizeof(logs) - used, format, args);

	assert(length >= 0 && (size_t)length < sizeof(logs) - used);
	assert(lines < sizeof(levels));
	levels[lines++] = level;
}

static void dev_info(void *device, const char *format, ...)
{
	va_list args;

	assert(!device);
	va_start(args, format);
	log_line('I', format, args);
	va_end(args);
}

static void dev_dbg(void *device, const char *format, ...)
{
	va_list args;

	assert(!device);
	va_start(args, format);
	log_line('D', format, args);
	va_end(args);
}

static void *cam_pointer(struct cam_device *device, unsigned long long address, size_t size)
{
	assert(device == &cam && address == CAM_PRINT_BASE && size == CAM_PRINT_SIZE);
	return mapped ? ring : NULL;
}

static char *strnstr(const char *text, const char *needle, size_t length)
{
	size_t i, n = strlen(needle);

	for (i = 0; i < length && text[i]; i++)
		if (n <= length - i && !memcmp(text + i, needle, n))
			return (char *)text + i;
	return NULL;
}

static void complete(struct completion *done)
{
	done->done++;
}

static void cam_fail(struct cam_device *device, const char *reason)
{
	assert(reason);
	device->dead = true;
	failures++;
}

static void cam_transport_fail(struct cam_device *device, const char *reason)
{
	device->transport_dead = true;
	cam_fail(device, reason);
}

#include ".build/print_impl.h"

static void reset(void)
{
	cam = (struct cam_device) { .fw_lock = true, .firmware_started = true,
		.print_cursor = CAM_PRINT_RECORD_SIZE };
	memset(ring, 0, sizeof(ring));
	logs[0] = 0;
	lines = 0;
	barriers = 0;
	writes = 0;
	failures = 0;
	calls = 0;
	cam_fw_log = false;
	mapped = true;
	if (limiter)
		limiter->used = 0;
}

static struct cam_print_record *slot(u32 cursor, unsigned int kind, unsigned int sequence)
{
	struct cam_print_record *record = (void *)(ring + cursor);

	memset(record, 0, sizeof(*record));
	record->kind = htole16(kind);
	record->sequence = htole16(sequence);
	return record;
}

static void producer(u32 cursor)
{
	*(__le32 *)ring = htole32(cursor);
}

static void kind_zero(void)
{
	unsigned int logging;

	for (logging = 0; logging <= 1; logging++) {
		struct cam_print_record *record;

		reset();
		cam_fw_log = logging;
		record = slot(CAM_PRINT_RECORD_SIZE, 0, 65535);
		memcpy(record->text, "eris SMP [ISP] CTRL READY", 26);
		producer(2 * CAM_PRINT_RECORD_SIZE);
		assert(!cam_print_poll(&cam));
		assert(!strcmp(logs, "ISP: eris SMP [ISP] CTRL READY\n"));
		assert(lines == 1 && levels[0] == (logging ? 'I' : 'D'));
		assert(cam.banners == 1 && cam.ready.done == 1 && !calls);
		assert(cam.print_cursor == 2 * CAM_PRINT_RECORD_SIZE && writes == 1 && barriers == 2);
		assert(le32_to_cpu(((__le32 *)ring)[1]) == cam.print_cursor);
		logs[0] = 0;
		cam_dead_print_dump(&cam);
		assert(!strcmp(logs, "DEAD: firmware print ring, 64 slots before producer=0x120 host=0x120\n"
			       "ISP[-1]: eris SMP [ISP] CTRL READY\n"));
		assert(!calls && cam.ready.done == 1 && cam.banners == 1);
		/* Kind 0 still detects a restart on its second banner. */
		cam.print_cursor = CAM_PRINT_RECORD_SIZE;
		assert(!cam_print_poll(&cam) && cam.transport_dead && failures == 1);
	}
}

static void other_kinds(void)
{
	static const unsigned int kinds[] = { 1, 2, 3, 4, 255, 65535 };
	unsigned int k;

	for (k = 0; k < ARRAY_SIZE(kinds); k++) {
		struct cam_print_record *record;
		char expected[256];

		reset();
		record = slot(CAM_PRINT_RECORD_SIZE, kinds[k], 0x1234);
		memcpy(record->text, "eris SMP [ISP] CTRL READY", 26);
		producer(2 * CAM_PRINT_RECORD_SIZE);
		assert(!cam_print_poll(&cam));
		snprintf(expected, sizeof(expected), "ISP: kind=%u seq=4660 eris SMP [ISP] CTRL READY\n",
			 kinds[k]);
		assert(!strcmp(logs, expected) && levels[0] == 'I');
		assert(!cam.banners && !cam.ready.done && !failures && calls == 1);
		logs[0] = 0;
		cam_dead_print_dump(&cam);
		snprintf(expected, sizeof(expected), "ISP[-1]: kind=%u seq=4660 eris SMP [ISP] CTRL READY\n",
			 kinds[k]);
		assert(strstr(logs, expected) && calls == 1 && !cam.banners && !cam.ready.done);
	}
}

static void text_and_hex_bounds(void)
{
	unsigned int variant, i;

	for (variant = 0; variant < 8; variant++) {
		struct cam_print_record *record;
		bool hex = variant >= 1 && variant <= 5;
		char expected[512], *at;

		reset();
		record = slot(CAM_PRINT_RECORD_SIZE, 3, 17);
		memset(record->text, 'A', sizeof(record->text));
		/* Printable ASCII, binary, leading NUL, DEL, a control byte and an
		 * inner newline; then console text ending in newlines and a bare one.
		 */
		if (variant == 1)
			record->text[127] = (char)0xff;
		if (variant == 2)
			record->text[0] = 0;
		if (variant == 3)
			record->text[127] = 0x7f;
		if (variant == 4)
			record->text[50] = 1;
		if (variant == 5)
			record->text[50] = '\n';
		if (variant == 6)
			memset(record->text + 100, '\n', 28);
		if (variant == 7)
			memcpy(record->text, "\n", 2);
		at = expected + sprintf(expected, "ISP: kind=3 seq=17 %s", hex ? "hex=" : "");
		for (i = 0; i < sizeof(record->text); i++) {
			if (hex)
				at += sprintf(at, "%02x", (u8)record->text[i]);
			else if (record->text[i] == 'A')
				*at++ = 'A';
			else
				break;
		}
		assert(at - expected < 300);
		*at++ = '\n';
		*at = 0;
		producer(2 * CAM_PRINT_RECORD_SIZE);
		assert(!cam_print_poll(&cam) && !strcmp(logs, expected));
		logs[0] = 0;
		cam_dead_print_dump(&cam);
		assert(strstr(logs, expected + strlen("ISP: ")));
	}
}

static void limit_and_dump(void)
{
	u8 before[CAM_PRINT_SIZE];
	unsigned int i, saved_calls;
	u32 cursor;

	reset();
	for (cursor = CAM_PRINT_RECORD_SIZE, i = 0; i < 64; i++, cursor = cam_print_next(cursor)) {
		struct cam_print_record *record = slot(cursor, 3, i);

		snprintf(record->text, sizeof(record->text), "assert %u", i);
	}
	producer(cursor);
	assert(!cam_print_poll(&cam) && lines == 10 && calls == 64);
	assert(cam.print_cursor == cursor && le32_to_cpu(((__le32 *)ring)[1]) == cursor);
	logs[0] = 0;
	lines = 0;
	saved_calls = calls;
	memcpy(before, ring, sizeof(before));
	cam_dead_print_dump(&cam);
	assert(lines == 65 && calls == saved_calls && !memcmp(before, ring, sizeof(before)));
	assert(strstr(logs, "ISP[-64]: kind=3 seq=0 assert 0\n"));
	assert(strstr(logs, "ISP[-1]: kind=3 seq=63 assert 63\n"));
	/* Exhausting the nonzero-kind limit never suppresses kind 0 or READY. */
	memcpy(slot(cursor, 0, 0)->text, "[ISP] CTRL READY", 17);
	producer(cam_print_next(cursor));
	assert(!cam_print_poll(&cam) && cam.ready.done == 1 && calls == saved_calls);
}

static void guards_and_wrap(void)
{
	unsigned int i;

	for (i = 0; i < 5; i++) {
		reset();
		producer(2 * CAM_PRINT_RECORD_SIZE);
		if (i == 0)
			mapped = false;
		if (i == 1)
			cam.print_failed = true;
		if (i == 2)
			cam.transport_dead = true;
		if (i == 3)
			producer(0);
		if (i == 4)
			producer(1);
		assert(!cam_print_poll(&cam) && !logs[0] && !writes && !calls);
		assert(failures == (i == 4 ? 1U : 0U));
	}
	reset();
	cam.print_cursor = CAM_PRINT_END - CAM_PRINT_RECORD_SIZE;
	memcpy(slot(cam.print_cursor, 3, 99)->text, "last", 5);
	memcpy(slot(CAM_PRINT_RECORD_SIZE, 1, 100)->text, "first", 6);
	producer(2 * CAM_PRINT_RECORD_SIZE);
	assert(!cam_print_poll(&cam) && lines == 2 && calls == 2);
	logs[0] = 0;
	cam_dead_print_dump(&cam);
	assert(strstr(logs, "ISP[-2]: kind=3 seq=99 last\n"));
	assert(strstr(logs, "ISP[-1]: kind=1 seq=100 first\n"));
}

int main(void)
{
	kind_zero();
	other_kinds();
	text_and_hex_bounds();
	limit_and_dump();
	guards_and_wrap();
	puts("print tests passed: byte-identical kind-0 live/DEAD text, kind/sequence tags, all 64 DEAD slots, newline-terminated text, binary/full-width bounds, ten-a-minute limit, cursor guards, wrap and kind-0-only READY/restart detection");
	return 0;
}
