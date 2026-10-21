/* SPDX-License-Identifier: GPL-2.0-only */
/* Production requests, transport reply policy and complete session setup run
 * on synthetic data. No firmware, sensor identifiers from captures or images.
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

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long long u64;
#ifndef _LINUX_TYPES_H
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
#endif
#define le16_to_cpu le16toh
#define le32_to_cpu le32toh
#define le64_to_cpu le64toh
#define cpu_to_le16 htole16
#define cpu_to_le32 htole32
#define cpu_to_le64 htole64
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BIT(n) (1U << (n))
#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_USEC 1000ULL
#define CAM_ALL_META ((1U << CAM_META_COUNT) - 1)
#define CAM_ALL_IMAGES ((1U << CAM_FRAME_COUNT) - 1)
#define CAM_VM_BASE 0x10000000000ULL
#define U64_MAX UINT64_MAX
#define __iomem
#define max(a, b) ((a) > (b) ? (a) : (b))
#define max_t(type, a, b) max((type)(a), (type)(b))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define READ_ONCE(x) ({ track_read(&(x)); (x); })
#define WRITE_ONCE(x, v) ((x) = (v))
#define spin_lock_irqsave(lock, flags) ((void)(lock), (flags) = 0)
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
#define lockdep_assert_held(lock) assert(*(lock))
#define dma_rmb() ((void)0)
#define dma_wmb() (barriers++)
#define time_before(a, b) ((a) < (b))
#define time_after_eq(a, b) ((a) >= (b))
#define msecs_to_jiffies(ms) (ms)
#define jiffies_to_msecs(ticks) (ticks)
#define TASK_INTERRUPTIBLE 1
#define TASK_RUNNING 0
#define scnprintf snprintf
#define strscpy(dst, src, size) snprintf(dst, size, "%s", src)
#define sysfs_emit(buffer, ...) snprintf(buffer, 4096, __VA_ARGS__)
#define hweight32 __builtin_popcount

#include "../j713_cam_wire.h"
#include "../j713_isp_d_range.h"
struct firmware { const u8 *data; size_t size; };
#include ".build/experiment_types.h"
enum cam_state { CAM_IDLE, CAM_STREAMING, CAM_DEAD };
struct completion { unsigned int done; };
struct kernel_param { void *arg; };
struct task_struct { int unused; };
struct vb2_queue { bool streaming, busy; };
struct cam_device {
	void *dev, *data;
	bool fw_lock, service_lock, firmware_running, shutting_down, query_stalled;
	bool query_pending, transport_dead;
	u8 pending_endpoint;
	unsigned int sessions, fw_sessions, frame_rate;
	enum cam_state state;
	struct cam_geometry geometry, format;
	u32 out_height;
	struct cam_session session;
	struct vb2_queue queue;
	struct cam_arena arena;
	struct task_struct *thread;
	struct completion reply, frames, stream_started;
	u64 capture_base, bt_iova, last_stop_ns;
	size_t capture_span;
	u32 sensor_version, presets, word_68, word_78;
};

static struct cam_device cam;
static struct cam_experiment_config cam_experiment_config;
static bool cam_experiment_lock;
static int cam_setfile_result, cam_lenscal_result;
static u8 command[CAM_QUERY_CAPACITY], blob_data[CAM_SETFILE_SIZE], *capture;
static struct cam_record record;
static struct firmware blob;
static struct task_struct task;
static u64 now_ns;
static unsigned long jiffies;
static unsigned int barriers, requests, releases, first_reads, failures, dumps, latches;
static unsigned int sent_barriers;
static unsigned int reject_at, reject_also_at, timeout_at, trace_count;
static u32 fps_min_reply, fps_max_reply;
static bool fps_clamp;
static unsigned int fps_zero_reads;
static unsigned int batches, stream_limit, parked, reclaimed, audited;
static int request_error;
static bool retune_at_send, retune_at_request;
static bool streaming, thread_stop, stop_before_requeue, batch_timeout;
static bool dead_at_requeue, returns_at_load;
static bool drop_frames;
static unsigned int output_model;
static bool output_pending;
static unsigned int dead_after_send, drift_at_requeue, framing_sends, limited_logs, suppressed_logs;
static u16 zoom_reply[2], reject_status;
static struct framing_event {
	u16 opcode;
	u32 delivered, requeues;
} framing_events[64];
static char requested[CAM_SETFILE_NAME_SIZE], logs[131072];
static struct trace {
	u32 endpoint, size;
	u64 argument;
	u8 command[CAM_QUERY_CAPACITY];
} trace[256];

static void track_read(const void *address)
{
	if (address == &cam.fw_sessions)
		first_reads++;
}

static void mutex_lock(bool *lock)
{
	assert(!*lock);
	*lock = true;
}

static void mutex_unlock(bool *lock)
{
	assert(*lock);
	*lock = false;
}

static u16 get_unaligned_le16(const void *p)
{
	u16 v;

	memcpy(&v, p, sizeof(v));
	return le16toh(v);
}

static u32 get_unaligned_le32(const void *p)
{
	u32 v;

	memcpy(&v, p, sizeof(v));
	return le32toh(v);
}

static u32 get_unaligned_be32(const void *p)
{
	u32 v;

	memcpy(&v, p, sizeof(v));
	return be32toh(v);
}

static u64 get_unaligned_le64(const void *p)
{
	uint64_t v;

	memcpy(&v, p, sizeof(v));
	return le64toh(v);
}

static void put_unaligned_le16(u16 v, void *p)
{
	v = htole16(v);
	memcpy(p, &v, sizeof(v));
}

static void put_unaligned_le32(u32 v, void *p)
{
	v = htole32(v);
	memcpy(p, &v, sizeof(v));
}

static void put_unaligned_le64(u64 v, void *p)
{
	uint64_t wire = htole64(v);

	memcpy(p, &wire, sizeof(wire));
}

static bool cam_dead(const struct cam_device *device)
{
	return device->state == CAM_DEAD;
}

static bool cam_can_send(const struct cam_device *device)
{
	return device->firmware_running && !device->transport_dead && !device->query_stalled;
}

static void *cam_pointer(struct cam_device *device, u64 address, size_t size)
{
	if (address == CAM_QUERY_COMMAND && size <= sizeof(command))
		return command;
	if (address == CAM_QUERY_RECORD && size <= sizeof(record))
		return &record;
	if (address >= device->capture_base &&
	    address - device->capture_base <= device->geometry.capture_size &&
	    size <= device->geometry.capture_size - (address - device->capture_base))
		return capture + address - device->capture_base;
	return NULL;
}

static void cam_fail(struct cam_device *device, const char *reason)
{
	assert(reason);
	if (!cam_dead(device))
		failures++;
	device->state = CAM_DEAD;
}

static void cam_dead_dump(struct cam_device *device)
{
	if (cam_dead(device))
		dumps++;
}

static void dev_info(void *device, const char *format, ...)
{
	va_list args;
	size_t used = strlen(logs);

	(void)device;
	va_start(args, format);
	/* libc treats the kernel hex-buffer formatter as a process address. */
	if (strstr(format, "%24phN")) {
		int result = va_arg(args, int);
		u32 words[5];
		const u8 *tail;
		char hex[49];
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(words); i++)
			words[i] = va_arg(args, unsigned int);
		tail = va_arg(args, const u8 *);
		for (i = 0; i < 24; i++)
			snprintf(hex + 2 * i, 3, "%02x", tail[i]);
		snprintf(logs + used, sizeof(logs) - used,
			 "lens-cal GET: result=%d status=%#x supported=%#x words=%08x %08x %08x tail=%s\n",
			 result, words[0], words[1], words[2], words[3], words[4], hex);
	} else {
		vsnprintf(logs + used, sizeof(logs) - used, format, args);
	}
	va_end(args);
}
#define dev_dbg dev_info

/* Old replay fixtures predate this host-only summary. Keep every other byte. */
static inline void strip_pacing_log(void)
{
	char line[160], *added;
	struct cam_session *s = &cam.session;

	snprintf(line, sizeof(line), "session %u pacing: gaps=%u avg=%lluus max=%lluus\n",
		 cam.sessions, s->pacing_gaps,
		 s->pacing_gaps ? s->pacing_gap_ns / s->pacing_gaps / NSEC_PER_USEC : 0,
		 s->pacing_max_gap_ns / NSEC_PER_USEC);
	added = strstr(logs, line);
	if (added)
		memmove(added, added + strlen(line), strlen(added + strlen(line)) + 1);
	assert(!strstr(logs, " pacing:"));
}

/* As lib/ratelimit.c: a burst per interval that starts at the first call. */
#define HZ 1000
struct ratelimit_state {
	unsigned long interval, begin;
	unsigned int burst, left;
	bool started;
};
#define DEFINE_RATELIMIT_STATE(name, interval_init, burst_init) \
	struct ratelimit_state name = { .interval = (interval_init), .burst = (burst_init) }
static struct ratelimit_state *ratelimit;

#if CAM_HAS_FRAMING
static int __ratelimit(struct ratelimit_state *state)
{
	assert(!ratelimit || ratelimit == state);
	ratelimit = state;
	limited_logs++;
	if (!state->started || time_before(state->begin + state->interval, jiffies)) {
		state->started = true;
		state->begin = jiffies;
		state->left = state->burst;
	}
	if (!state->left) {
		suppressed_logs++;
		return 0;
	}
	state->left--;
	return 1;
}
#endif

static void reinit_completion(struct completion *done)
{
	done->done = 0;
}

static void complete(struct completion *done)
{
	done->done++;
}

static bool kthread_should_stop(void)
{
	return thread_stop || cam.session.requeues >= stream_limit;
}

static void set_current_state(int state)
{
	assert(state == TASK_INTERRUPTIBLE && cam.fw_lock);
}

static void __set_current_state(int state)
{
	assert(state == TASK_RUNNING && cam.fw_lock);
}

static void schedule(void)
{
	assert(cam.session.stopped && cam.fw_lock);
	parked++;
	thread_stop = true;
}

static void cam_deliver(struct cam_device *device, unsigned int slot, u64 timestamp)
{
	assert(device->fw_lock && streaming && slot < CAM_FRAME_COUNT && timestamp);
	device->session.sequence++;
	if (!drop_frames)
		device->session.delivered++;
}

static void cam_publish_status(struct cam_device *device)
{
	(void)device;
}

static void cam_print_drain(struct cam_device *device)
{
	struct cam_print_record message = { 0 };

	if (!output_pending || !device->session.output.watching)
		return;
	output_pending = false;
	if (output_model == 1)
		return;
	if (output_model >= 4 && device->session.output.seen) {
		snprintf(message.text, sizeof(message.text),
			 "MaxFESOutput was (1920, 1888), Replaced with (1920, 1080)");
		cam_output_record(&device->session.output, &message);
		return;
	}
	snprintf(message.text, sizeof(message.text),
		 "[MSC] CH = 0x0 BES[0] originalOutputSize [1920 %u]\n",
		 output_model == 2 ? 1080 : 1888);
	message.dropped = cpu_to_le16(output_model == 6);
	cam_output_record(&device->session.output, &message);
	if (output_model == 3) {
		snprintf(message.text, sizeof(message.text),
			 "MaxFESOutput was (1920, 1888), Replaced with (1920, 1080)");
		cam_output_record(&device->session.output, &message);
	}
}

static u64 ktime_get_ns(void)
{
	return now_ns += NSEC_PER_MSEC;
}

static void cam_delay(struct cam_device *device, unsigned int duration_ms)
{
	(void)device;
	jiffies += duration_ms;
	if (streaming && duration_ms == CAM_STREAM_SETTLE_MS && stop_before_requeue)
		device->session.stop_requested = true;
}

static void cam_reclaim(struct cam_device *device)
{
	assert(!cam_dead(device));
	reclaimed++;
}

static void cam_guard_audit(struct cam_device *device)
{
	assert(!cam_dead(device));
	audited++;
}

static bool cam_all_back(struct cam_device *device)
{
	return device->session.acked_meta == CAM_ALL_META &&
		device->session.acked_images == CAM_ALL_IMAGES &&
		device->session.acked_capture == CAM_ALL_META;
}

static int cam_stream_thread(void *data);

static struct task_struct *kthread_run(int (*function)(void *), void *data, const char *name)
{
	assert(function == cam_stream_thread && data == &cam && !strcmp(name, "j713-camera"));
	return &task;
}

static int cam_lenscal_get(struct cam_device *device);
static int cam_lenscal_set(struct cam_device *device, unsigned int number);
static int cam_setfile_load(struct cam_device *device);
#if CAM_HAS_FRAMING
static int cam_framing_crop(struct cam_device *device, const char *when);
static int cam_framing_zoom(struct cam_device *device, const char *when, u16 zoom[2]);
static int cam_framing_pin(struct cam_device *device);
#endif
static int cam_exchange(struct cam_device *device, u8 endpoint, size_t insize, size_t outsize);
static int cam_message(struct cam_device *device, u8 endpoint, size_t size, u64 argument);

static void retune(void)
{
	/* Simulate a parameter write after the one locked read. */
	cam_experiment_config = (struct cam_experiment_config) { };
}

static int request_firmware(const struct firmware **result, const char *name, void *device)
{
	assert(device == cam.dev && cam.fw_lock && !cam_experiment_lock && !trace_count);
	requests++;
	snprintf(requested, sizeof(requested), "%s", name);
	if (retune_at_request)
		retune();
	if (request_error)
		return request_error;
	*result = &blob;
	return 0;
}

static void release_firmware(const struct firmware *data)
{
	assert(data == &blob);
	releases++;
}

/* Every image comes back, recorded as the receive worker records it. */
static void return_batch(struct cam_device *device)
{
	unsigned int i;

	device->session.acked_images = CAM_ALL_IMAGES;
	device->session.acked_meta = CAM_ALL_META;
	device->session.completed = CAM_ALL_IMAGES;
	device->session.order_count = CAM_FRAME_COUNT;
	for (i = 0; i < CAM_FRAME_COUNT; i++) {
		device->session.order[i] = i;
		device->session.timestamps[i] = ktime_get_ns();
	}
	complete(&device->frames);
}

static int cam_send(struct cam_device *device, u8 endpoint, u64 address, const char *name)
{
	struct trace *t = &trace[trace_count++];
	u16 opcode = get_unaligned_le16(command + 4);

	assert(trace_count <= ARRAY_SIZE(trace) && address == CAM_QUERY_RECORD && name);
	assert(!cam_experiment_lock);
	assert(device->fw_lock);
	if (endpoint == 0x21 && (opcode == 0x519 || opcode == 0x50f))
		assert(cam_lenscal_result == -EINPROGRESS);
	if (endpoint == 0x21 && opcode == 0x111) {
		assert(cam_setfile_result == -EINPROGRESS && barriers == sent_barriers + 2);
		if (!device->session.experiment.colour_active)
			assert(!memcmp(capture + CAM_ALIGN16K(device->geometry.pool_end), blob_data,
				       CAM_SETFILE_SIZE));
	}
	if (endpoint == 0x21 && (opcode == 0x800 || opcode == 0x811 || opcode == 0x810)) {
		struct framing_event *event = &framing_events[framing_sends++];

		assert(!cam_dead(device) && framing_sends <= ARRAY_SIZE(framing_events));
		*event = (struct framing_event) { opcode, device->session.delivered,
			device->session.requeues };
	}
	sent_barriers = barriers;
	*t = (struct trace) { .endpoint = endpoint, .size = le64_to_cpu(record.arg[1]),
		.argument = le64_to_cpu(record.arg[2]) };
	memcpy(t->command, command, sizeof(command));
	if (retune_at_send)
		retune();
	if (endpoint == 0x24) {
		record.arg[2] = cpu_to_le64(CAM_BUFFER_ACK);
		if (output_model == 5 && device->session.capture_issued)
			output_pending = true;
		/* A bad return latches DEAD while the requeue's reply still arrives. */
		if (streaming && dead_at_requeue)
			cam_fail(device, "invalid service record");
		if (streaming && drift_at_requeue == device->session.requeues + 1) {
			zoom_reply[0] = 0x600;
			zoom_reply[1] = 0x401;
		}
	} else if (opcode == 0x207) {
		/* Bits 0/1 make MAX_GET/MIN_GET read 0 while the modelled range stays. */
		put_unaligned_le32(fps_zero_reads & 1 ? 0 : fps_max_reply, command + 0xc);
	} else if (opcode == 0x209) {
		put_unaligned_le32(fps_zero_reads & 2 ? 0 : fps_min_reply, command + 0xc);
	} else if ((opcode == 0x260 || opcode == 0x208 || opcode == 0x20a) &&
		   reject_at != trace_count && reject_also_at != trace_count &&
		   timeout_at != trace_count) {
		u32 rate = fps_clamp ? 30 * 256 : opcode == 0x260 ?
			get_unaligned_le32(command + 0x10) * 256 : get_unaligned_le32(command + 0xc);

		if (opcode != 0x208)
			fps_min_reply = rate;
		if (opcode != 0x20a)
			fps_max_reply = rate;
		/* The firmware's SetMinMaxFrameRate asserts min <= max. */
		assert(fps_min_reply <= fps_max_reply);
		if (output_model == 4)
			output_pending = true;
	} else if (opcode == 0x800) {
		unsigned int i;

		for (i = 0; i < 8; i++)
			put_unaligned_le32(0x10203040U + i * 0x01010101U, command + 0xc + 4 * i);
	} else if (opcode == 0x811) {
		put_unaligned_le16(zoom_reply[0], command + 0xc);
		put_unaligned_le16(zoom_reply[1], command + 0xe);
	} else if (opcode == 0x810 && reject_at != trace_count && timeout_at != trace_count) {
		zoom_reply[0] = 0x400;
		zoom_reply[1] = 0x400;
	} else if (opcode == 0x10d) {
		put_unaligned_le32(0x958, command + 0x20);
		put_unaligned_le32(1, command + 0x60);
		put_unaligned_le32(0x100, command + 0x68);
		put_unaligned_le32(0x200, command + 0x78);
	} else if (opcode == 0x105) {
		put_unaligned_le16(1280, command + 0x10);
		put_unaligned_le16(1264, command + 0x12);
		put_unaligned_le16(1280, command + 0x14);
		put_unaligned_le16(device->geometry.height, command + 0x16);
	} else if (opcode == 0xb01) {
		output_pending = true;
	} else if (opcode == 0x104) {
		device->session.acked_meta = CAM_ALL_META;
		device->session.acked_capture = CAM_ALL_META;
		device->session.acked_images = CAM_ALL_IMAGES;
	} else if (opcode == 0x111 && returns_at_load) {
		return_batch(device);
	}
	if (reject_at == trace_count || reject_also_at == trace_count) {
		put_unaligned_le16(reject_status, command + 6);
		put_unaligned_le32(0xa1b2c3d4, command + 0xc);
	}
	return 0;
}

static bool cam_wait(struct cam_device *device, struct completion *done,
		     unsigned int timeout_ms, bool stopping)
{
	if (done == &cam.frames) {
		assert(streaming && device->fw_lock && timeout_ms == 1000 && !stopping);
		if (batch_timeout)
			return false;
		batches++;
		return_batch(device);
		return true;
	}
	assert(device == &cam && done == &cam.reply && timeout_ms == 2000 && stopping);
	if (dead_after_send == trace_count)
		cam_fail(device, "invalid service record during command");
	return timeout_at != trace_count;
}

/* Definitions in production are externally linked; prototypes above match. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include ".build/experiment_impl.h"
#pragma GCC diagnostic pop

static void reset(unsigned int height)
{
	cam = (struct cam_device) {
		.fw_lock = true, .firmware_running = true, .capture_base = 0x10040000000ULL,
		.bt_iova = 0x10020000000ULL, .capture_span = CAM_CAPTURE_MAX,
	};
	assert(cam_geometry_init(&cam.geometry, height));
	cam.format = cam.geometry;
	cam.out_height = height == 1888 ? 1264 : height;
	/* Knobs off, the pre-2026-10-18 load default. fps_test.c checks the
	 * production defaults (colour=1 framing=2 fps=1) against these.
	 */
	cam_experiment_config = (struct cam_experiment_config) { };
	memset(command, 0xa5, sizeof(command));
	memset(capture, 0xa5, height == 1888 ? CAM_CAPTURE_MAX : CAM_CAPTURE_SIZE);
	memset(&record, 0xa5, sizeof(record));
	memset(blob_data, 0, sizeof(blob_data));
	put_unaligned_le32(htobe32(0x22240106), blob_data);
	put_unaligned_le32(0x20, blob_data + 0x10);
	put_unaligned_le32(0x34, blob_data + 0x6fbc);
	put_unaligned_le16(0x50f, blob_data + 0x6fc4);
	memset(blob_data + 0x6fcc, 0x5a, 0x28);
	put_unaligned_le32(0x958, blob_data + 0x6fd4);
	blob = (struct firmware) { .data = blob_data, .size = sizeof(blob_data) };
	barriers = 0;
	sent_barriers = 0;
	requests = 0;
	releases = 0;
	first_reads = 0;
	failures = 0;
	dumps = 0;
	latches = 0;
	reject_at = 0;
	reject_also_at = 0;
	fps_min_reply = 15 * 256;
	fps_max_reply = 60 * 256;
	fps_clamp = false;
	fps_zero_reads = 0;
	timeout_at = 0;
	trace_count = 0;
	batches = 0;
	stream_limit = 2;
	parked = 0;
	reclaimed = 0;
	audited = 0;
	streaming = false;
	thread_stop = false;
	stop_before_requeue = false;
	batch_timeout = false;
	dead_at_requeue = false;
	returns_at_load = false;
	drop_frames = false;
	output_model = 0;
	output_pending = false;
	dead_after_send = 0;
	drift_at_requeue = 0;
	framing_sends = 0;
	limited_logs = 0;
	suppressed_logs = 0;
	if (ratelimit)
		ratelimit->started = false;
	reject_status = 0xdead;
	zoom_reply[0] = 0x456;
	zoom_reply[1] = 0x789;
	memset(framing_events, 0, sizeof(framing_events));
	request_error = 0;
	retune_at_send = false;
	retune_at_request = false;
	now_ns = 0;
	jiffies = 0;
	logs[0] = 0;
	requested[0] = 0;
	cam_setfile_result = 0;
	cam_lenscal_result = 0;
}

/* The real kthread takes fw_lock after the session-start worker drops it. */
static void run_stream(void)
{
	assert(cam.thread && cam.fw_lock);
	mutex_unlock(&cam.fw_lock);
	streaming = true;
	assert(!cam_stream_thread(&cam));
	streaming = false;
	assert(!cam.fw_lock && cam.session.stopped);
	mutex_lock(&cam.fw_lock);
	cam.thread = NULL;
}

static unsigned int count_opcode(u16 opcode)
{
	unsigned int i, count = 0;

	for (i = 0; i < trace_count; i++)
		count += trace[i].endpoint == 0x21 &&
			get_unaligned_le16(trace[i].command + 4) == opcode;
	return count;
}

static unsigned int index_opcode(u16 opcode, unsigned int occurrence)
{
	unsigned int i;

	for (i = 0; i < trace_count; i++)
		if (trace[i].endpoint == 0x21 &&
		    get_unaligned_le16(trace[i].command + 4) == opcode && !occurrence--)
			return i;
	abort();
}
