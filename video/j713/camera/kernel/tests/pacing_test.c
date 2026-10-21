// SPDX-License-Identifier: GPL-2.0-only
/* Real batch wait, stream loop and V4L2 delivery on synthetic host buffers. */
#include <assert.h>
#include <endian.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef unsigned long long u64;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
#define le16_to_cpu le16toh
#define le32_to_cpu le32toh
#define le64_to_cpu le64toh
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_USEC 1000ULL
#define CAM_ALL_IMAGES ((1U << CAM_FRAME_COUNT) - 1)
#define CAM_ALL_META ((1U << CAM_META_COUNT) - 1)
#define max(a, b) ((a) > (b) ? (a) : (b))
#define max_t(type, a, b) max((type)(a), (type)(b))
#define READ_ONCE(value) (value)
#define time_after_eq(a, b) ((a) >= (b))
#define msecs_to_jiffies(ms) (ms)
#define jiffies_to_msecs(ticks) (ticks)
#define hweight32 __builtin_popcount
#define TASK_INTERRUPTIBLE 1
#define TASK_RUNNING 0
#define V4L2_FIELD_NONE 1
#define dev_dbg(...) ((void)0)
#define dev_info(...) ((void)0)
#include "../j713_cam_wire.h"
#include ".build/experiment_types.h"

struct completion { unsigned int count; };
struct list_head { bool available; };
enum vb2_buffer_state { VB2_BUF_STATE_DONE, VB2_BUF_STATE_ERROR };
struct vb2_buffer { u64 timestamp; size_t payload; };
struct cam_buffer_vb2 {
	struct {
		struct vb2_buffer vb2_buf;
		u32 sequence, field;
	} vb;
	struct list_head list;
};
struct cam_device {
	void *dev;
	bool service_lock, buffers_lock, fw_lock, dead, auto_white_balance;
	struct cam_session session;
	struct cam_geometry geometry;
	u64 capture_base;
	struct list_head buffers;
	struct completion frames, stream_started;
	int queue;
};

static struct cam_device cam;
static struct cam_buffer_vb2 test_buffer;
static u8 capture[CAM_CAPTURE_MAX], dest[1920 * 1080 * 3 / 2];
static unsigned int test_height = 1264;
static u64 now_ns;
static unsigned long jiffies, deadline;
static unsigned int waits, event_count, done_count, errors, barriers, stage, queue_errors;
static unsigned int stop_after_requeues, expected_at_requeue, source_slot;
static u32 expected_meta;
static int bad_source;
static bool bad_dest;
static char failure[80];
static struct delivery {
	u64 host_ns, timestamp;
	u32 slot, sequence;
} deliveries[16];
static struct event {
	unsigned int ms, expected_done;
	u32 returned, completed, acked;
	bool dead;
} events[16];

static void take(bool *lock)
{
	assert(!*lock);
	*lock = true;
}

static void release(bool *lock)
{
	assert(*lock);
	*lock = false;
}
#define spin_lock_irqsave(lock, flags) do { take(lock); (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(flags); release(lock); } while (0)
#define mutex_lock take
#define mutex_unlock release
#define list_empty(head) (!(head)->available)
#define list_first_entry(head, type, member) \
	(assert((head)->available), (void)offsetof(type, member), &test_buffer)

static void list_del_init(struct list_head *head)
{
	assert(head == &test_buffer.list && cam.buffers_lock);
	cam.buffers.available = false;
}

static bool cam_dead(const struct cam_device *device)
{
	return device->dead;
}

static void cam_fail(struct cam_device *device, const char *reason)
{
	snprintf(failure, sizeof(failure), "%s", reason);
	device->dead = true;
}

static void *cam_pointer(struct cam_device *device, u64 address, size_t size)
{
	assert(device->fw_lock && !device->service_lock && !device->buffers_lock);
	assert(size == device->geometry.frame_stride && address >= device->capture_base);
	source_slot = (address - device->capture_base - CAM_RENDER_OFFSET) / size;
	assert(source_slot < CAM_FRAME_COUNT);
	return (int)source_slot == bad_source ? NULL : capture + address - device->capture_base;
}

static void vb2_queue_error(int *queue)
{
	assert(queue == &cam.queue && cam.fw_lock && !cam.buffers_lock);
	queue_errors++;
}

static void *vb2_plane_vaddr(struct vb2_buffer *vb, unsigned int plane)
{
	assert(vb == &test_buffer.vb.vb2_buf && !plane);
	return bad_dest ? NULL : dest;
}

static void vb2_set_plane_payload(struct vb2_buffer *vb, unsigned int plane, size_t size)
{
	assert(vb == &test_buffer.vb.vb2_buf && !plane && size == cam.geometry.deliver_size);
	vb->payload = size;
}

static void vb2_buffer_done(struct vb2_buffer *vb, enum vb2_buffer_state state)
{
	assert(vb == &test_buffer.vb.vb2_buf && cam.fw_lock && !cam.dead);
	assert(!cam.service_lock && !cam.buffers_lock);
	cam.buffers.available = true;
	if (state == VB2_BUF_STATE_ERROR) {
		errors++;
		return;
	}
	assert(done_count < ARRAY_SIZE(deliveries) && vb->payload == cam.geometry.deliver_size);
	assert(test_buffer.vb.field == V4L2_FIELD_NONE && dest[0] == 0x30 + source_slot);
	deliveries[done_count++] = (struct delivery) {
		now_ns, vb->timestamp, source_slot, test_buffer.vb.sequence,
	};
}

static u64 ktime_get_ns(void)
{
	return now_ns;
}

static void dma_rmb(void)
{
	assert(cam.fw_lock && !cam.service_lock && !cam.buffers_lock);
	barriers++;
}

static void complete(struct completion *done)
{
	done->count++;
}

/* Model separate return and ACK publications, including stale wakeups. */
static void publish(const struct event *event)
{
	unsigned int slot;

	take(&cam.service_lock);
	for (slot = 0; slot < CAM_FRAME_COUNT; slot++) {
		if (!(event->returned & BIT(slot)))
			continue;
		assert(cam.session.order_count < CAM_FRAME_COUNT);
		cam.session.order[cam.session.order_count++] = slot;
		cam.session.timestamps[slot] = now_ns + 123;
	}
	cam.session.completed |= event->completed;
	cam.session.acked_images |= event->acked;
	cam.dead |= event->dead;
	release(&cam.service_lock);
}

static bool cam_wait(struct cam_device *device, struct completion *done,
		     unsigned int timeout_ms, bool stopping)
{
	const struct event *event;

	assert(device == &cam && done == &cam.frames && cam.fw_lock && !stopping);
	assert(!cam.service_lock && jiffies + timeout_ms == deadline);
	if (waits == event_count) {
		jiffies += timeout_ms;
		now_ns = jiffies * NSEC_PER_MSEC;
		return false;
	}
	event = &events[waits++];
	assert(done_count == event->expected_done && time_after_eq(event->ms, jiffies));
	assert(event->ms < deadline);
	jiffies = event->ms;
	now_ns = jiffies * NSEC_PER_MSEC;
	publish(event);
	return true;
}

static void cam_delay(struct cam_device *device, unsigned int duration_ms)
{
	assert(device == &cam && device->fw_lock && duration_ms == CAM_STREAM_SETTLE_MS);
	assert(!stage && device->session.acked_images == CAM_ALL_IMAGES);
	stage = 1;
	jiffies += duration_ms;
	now_ns = jiffies * NSEC_PER_MSEC;
}

static bool kthread_should_stop(void)
{
	return cam.session.stopped || cam.session.requeues >= stop_after_requeues;
}

static int cam_requeue(struct cam_device *device, u32 meta, u32 images)
{
	assert(device == &cam && device->fw_lock && !cam_dead(device));
	assert(stage == 1 && meta == expected_meta && images == CAM_ALL_IMAGES);
	assert(device->session.acked_images == CAM_ALL_IMAGES && done_count == expected_at_requeue);
	stage = 2;
	device->session.acked_images = 0;
	device->session.completed = 0;
	device->session.order_count = 0;
	device->session.requeues++;
	deadline = jiffies + 1000;
	return 0;
}

static int cam_framing_requeue(struct cam_device *device)
{
	assert(device == &cam && stage == 2);
	stage = 0;
	return 0;
}

static void cam_session_stop(struct cam_device *device)
{
	assert(device == &cam && device->fw_lock && !device->session.stopped);
	assert(device->dead || stage == 1);
	device->session.stopped = true;
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
	assert(false);
}

#include ".build/pacing_impl.h"

static void reset(void)
{
	unsigned int slot;

	cam = (struct cam_device) { .capture_base = 0x10040000000ULL };
	assert(cam_geometry_init(&cam.geometry, test_height));
	cam.buffers.available = true;
	test_buffer = (struct cam_buffer_vb2) { };
	for (slot = 0; slot < CAM_FRAME_COUNT; slot++)
		memset(capture + CAM_RENDER_OFFSET + slot * cam.geometry.frame_stride,
		       0x30 + slot, cam.geometry.frame_stride);
	waits = 0;
	event_count = 0;
	done_count = 0;
	errors = 0;
	barriers = 0;
	stage = 0;
	now_ns = 0;
	jiffies = 0;
	deadline = 1000;
	stop_after_requeues = 0;
	expected_at_requeue = 2;
	expected_meta = 0;
	bad_source = -1;
	bad_dest = false;
	failure[0] = 0;
}

static void ready_slots(void)
{
	u8 order[] = { 1, 0 }, ready[CAM_FRAME_COUNT];
	u32 delivered = 0;

	assert(!cam_ready_slots(order, 0, CAM_ALL_IMAGES, CAM_ALL_IMAGES, &delivered, ready));
	assert(!cam_ready_slots(order, 1, BIT(1), 0, &delivered, ready) && !delivered);
	assert(cam_ready_slots(order, 1, BIT(1), BIT(1), &delivered, ready) == 1);
	assert(ready[0] == 1 && delivered == BIT(1));
	assert(!cam_ready_slots(order, 1, BIT(1), BIT(1), &delivered, ready));
	assert(cam_ready_slots(order, 2, CAM_ALL_IMAGES, CAM_ALL_IMAGES, &delivered, ready) == 1);
	assert(ready[0] == 0 && delivered == CAM_ALL_IMAGES);
	assert(!cam_ready_slots(order, 2, CAM_ALL_IMAGES, CAM_ALL_IMAGES, &delivered, ready));
	/* A requeue starts another batch, even if the order is unchanged. */
	delivered = 0;
	assert(cam_ready_slots(order, 2, CAM_ALL_IMAGES, CAM_ALL_IMAGES, &delivered, ready) == 2);
	assert(ready[0] == 1 && ready[1] == 0 && delivered == CAM_ALL_IMAGES);
	delivered = 0;
	assert(cam_ready_slots(order, 2, BIT(0), CAM_ALL_IMAGES, &delivered, ready) == 1);
	assert(ready[0] == 0 && delivered == BIT(0));
	/* Even an out-of-order ACK must not let a later render overtake. */
	delivered = 0;
	assert(!cam_ready_slots(order, 2, CAM_ALL_IMAGES, BIT(0), &delivered, ready));
	assert(!delivered);
}

static void immediate_delivery(void)
{
	reset();
	/* Stop is requested before either slot is back, and waits for both. */
	cam.session.stop_requested = true;
	events[0] = (struct event) { 1, 0, BIT(1), BIT(1), 0, false };
	events[1] = (struct event) { 2, 0, 0, 0, 0, false };
	events[2] = (struct event) { 3, 0, 0, 0, BIT(1), false };
	events[3] = (struct event) { 4, 1, 0, 0, 0, false };
	events[4] = (struct event) { 34, 1, BIT(0), BIT(0), 0, false };
	events[5] = (struct event) { 35, 1, 0, 0, BIT(0), false };
	event_count = 6;
	assert(!cam_stream_thread(&cam) && !cam.fw_lock && cam.session.stopped && !cam.dead);
	assert(waits == 6 && done_count == 2 && barriers == 2 && !cam.session.requeues);
	assert(deliveries[0].slot == 1 && deliveries[0].host_ns == 3 * NSEC_PER_MSEC);
	assert(deliveries[0].timestamp == NSEC_PER_MSEC + 123 && !deliveries[0].sequence);
	assert(deliveries[1].slot == 0 && deliveries[1].host_ns == 35 * NSEC_PER_MSEC);
	assert(deliveries[1].timestamp == 34 * NSEC_PER_MSEC + 123 && deliveries[1].sequence == 1);
	assert(cam.session.pacing_gaps == 1 && cam.session.pacing_gap_ns == 32 * NSEC_PER_MSEC);
	assert(cam.session.pacing_max_gap_ns == 32 * NSEC_PER_MSEC);
}

static void batch_remainder(void)
{
	reset();
	cam.fw_lock = true;
	cam.session.order[0] = 1;
	cam.session.order[1] = 0;
	cam.session.order_count = 2;
	cam.session.timestamps[0] = 10;
	cam.session.timestamps[1] = 20;
	cam.session.completed = CAM_ALL_IMAGES;
	cam.session.acked_images = CAM_ALL_IMAGES;
	assert(cam_batch(&cam) && !waits && done_count == 2);
	assert(deliveries[0].slot == 1 && deliveries[1].slot == 0);
	reset();
	cam.session.order[0] = 1;
	cam.session.order[1] = 0;
	cam.session.order_count = 2;
	cam.session.completed = BIT(0);
	cam.session.acked_images = CAM_ALL_IMAGES;
	assert(!cam_stream_thread(&cam) && done_count == 1 && deliveries[0].slot == 0);
}

static void requeue_boundary(void)
{
	const u32 metadata[] = { BIT(0), 0x7ff, 0xfff, CAM_ALL_META };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(metadata); i++) {
		reset();
		stop_after_requeues = 1;
		cam.session.acked_meta = metadata[i];
		expected_meta = i < 2 ? 0 : metadata[i];
		events[0] = (struct event) { 0, 0, BIT(1), BIT(1), BIT(1), false };
		events[1] = (struct event) { 33, 1, BIT(0), BIT(0), BIT(0), false };
		events[2] = (struct event) { 66, 2, BIT(0), BIT(0), BIT(0), false };
		events[3] = (struct event) { 99, 3, BIT(1), BIT(1), BIT(1), false };
		event_count = 4;
		assert(!cam_stream_thread(&cam) && done_count == 4 && cam.session.requeues == 1);
		assert(waits == 4 && stage == 1 && !cam.dead && now_ns == 109 * NSEC_PER_MSEC);
		assert(cam.session.pacing_gaps == 3 && cam.session.pacing_gap_ns == 99 * NSEC_PER_MSEC);
		assert(cam.session.pacing_max_gap_ns == 33 * NSEC_PER_MSEC);
	}

}

static void dead_and_timeout(void)
{
	reset();
	events[0] = (struct event) { 1, 0, BIT(1), BIT(1), BIT(1), false };
	events[1] = (struct event) { 34, 1, BIT(0), BIT(0), BIT(0), true };
	event_count = 2;
	assert(!cam_stream_thread(&cam) && cam.dead && done_count == 1 && !stage);
	reset();
	cam.dead = true;
	cam.session.acked_images = CAM_ALL_IMAGES;
	assert(!cam_stream_thread(&cam) && !done_count && !waits);
	reset();
	bad_source = 0;
	events[0] = (struct event) { 1, 0, CAM_ALL_IMAGES, CAM_ALL_IMAGES, CAM_ALL_IMAGES, false };
	event_count = 1;
	assert(!cam_stream_thread(&cam) && cam.dead && !done_count && errors == 1);
	assert(cam.session.sequence == 1 && !cam.session.pacing_gaps);
	/* DEAD from the batch's last copy: no settle, requeue, S5 or framing. */
	reset();
	bad_source = 1;
	events[0] = (struct event) { 1, 0, CAM_ALL_IMAGES, CAM_ALL_IMAGES, CAM_ALL_IMAGES, false };
	event_count = 1;
	assert(!cam_stream_thread(&cam) && cam.dead && done_count == 1 && errors == 1);
	assert(deliveries[0].slot == 0 && !stage && !cam.session.requeues);
	reset();
	events[0] = (struct event) { 1, 0, BIT(1), BIT(1), BIT(1), false };
	events[1] = (struct event) { 999, 1, 0, 0, 0, false };
	event_count = 2;
	assert(!cam_stream_thread(&cam) && cam.dead && done_count == 1);
	assert(now_ns == 1000 * NSEC_PER_MSEC);
	assert(!strcmp(failure, "batch timeout (1 second)"));
}

static void successful_deliveries_only(void)
{
	reset();
	cam.fw_lock = true;
	cam_deliver(&cam, 0, 11);
	assert(done_count == 1 && !cam.session.pacing_gaps && !cam.session.last_delivery_ns);
	cam.buffers.available = false;
	now_ns = 10 * NSEC_PER_MSEC;
	cam_deliver(&cam, 1, 22);
	assert(done_count == 1 && cam.session.dropped == 1 && !cam.session.pacing_gaps);
	cam.buffers.available = true;
	now_ns = 30 * NSEC_PER_MSEC;
	cam_deliver(&cam, 1, 33);
	assert(done_count == 2 && cam.session.pacing_gaps == 1);
	assert(cam.session.pacing_gap_ns == 30 * NSEC_PER_MSEC);
	assert(cam.session.pacing_max_gap_ns == 30 * NSEC_PER_MSEC);
	/* A shorter later gap adds to the sum but keeps the earlier maximum. */
	now_ns = 35 * NSEC_PER_MSEC;
	cam_deliver(&cam, 0, 35);
	assert(done_count == 3 && cam.session.pacing_gaps == 2);
	assert(cam.session.pacing_gap_ns == 35 * NSEC_PER_MSEC);
	assert(cam.session.pacing_max_gap_ns == 30 * NSEC_PER_MSEC);
	bad_dest = true;
	now_ns = 40 * NSEC_PER_MSEC;
	cam_deliver(&cam, 0, 44);
	assert(errors == 1 && cam.dead && done_count == 3 && cam.session.pacing_gaps == 2);
	assert(cam.session.last_delivery_ns == 35 * NSEC_PER_MSEC);
}

static void late_output_replacement(void)
{
	reset();
	cam.fw_lock = true;
	cam.session.output.rejected = true;
	queue_errors = 0;
	cam_deliver(&cam, 0, 11);
	assert(!done_count && errors == 1 && !barriers && !cam.dead && queue_errors == 1);
	assert(cam.session.stop_requested && cam.session.output_rejected && cam.session.dropped == 1);
}

int main(void)
{
	/* Referenced here so a deleted call fails at runtime, not as unused. */
	(void)vb2_queue_error;
	ready_slots();
	immediate_delivery();
	batch_remainder();
	requeue_boundary();
	dead_and_timeout();
	successful_deliveries_only();
	late_output_replacement();
	test_height = 1888;
	immediate_delivery();
	batch_remainder();
	requeue_boundary();
	dead_and_timeout();
	successful_deliveries_only();
	late_output_replacement();
	puts("pacing tests passed: both sizes, immediate ACKed renders, return order, stale wakeups, batch remainder/reset, non-render exclusion, settle/requeue/metadata/framing order, stop boundary, DEAD, one-second deadline, DONE-only host gaps and firmware timestamps, late output rejection");
	return 0;
}
