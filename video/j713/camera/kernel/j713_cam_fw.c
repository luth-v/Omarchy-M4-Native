// SPDX-License-Identifier: GPL-2.0-only
#include <linux/slab.h>
#include <linux/stdarg.h>

#include "j713_cam.h"
#include "j713_isp_d_range.h"

void *cam_pointer(struct cam_device *cam, u64 address, size_t size)
{
	size_t offset;

	if (cam->identity && d_range_offset(address, cam->window_base,
					  CAM_IDENTITY_SPAN(cam), size, &offset))
		return cam->identity + PAGE_SIZE + offset;
	if (!cam->ram || !d_range_offset(address, CAM_VM_BASE + CAM_FW_END,
					cam->ram_size, size, &offset))
		return NULL;
	return cam->ram + offset;
}

static void cam_transport_fail(struct cam_device *cam, const char *reason)
{
	unsigned long flags;

	spin_lock_irqsave(&cam->service_lock, flags);
	WRITE_ONCE(cam->transport_dead, true);
	spin_unlock_irqrestore(&cam->service_lock, flags);
	cam_fail(cam, reason);
}

void cam_failf(struct cam_device *cam, const char *format, ...)
{
	char reason[sizeof(cam->dead_reason)];
	va_list args;

	va_start(args, format);
	vscnprintf(reason, sizeof(reason), format, args);
	va_end(args);
	cam_fail(cam, reason);
}

static int cam_send(struct cam_device *cam, u8 endpoint, u64 address, const char *name)
{
	unsigned long flags;
	u32 control;
	int ret;

	if (!cam_can_send(cam))
		return -EIO;
	/* Poll with IRQs on, then use the mailbox's bounded atomic send. */
	ret = readl_poll_timeout(cam->mbox->regs + 0x110, control,
				 !(control & BIT(16)), 100, 500000);
	if (!ret) {
		/* Serialize the last send check with a crash/restart latch. */
		spin_lock_irqsave(&cam->service_lock, flags);
		if (!cam_can_send(cam))
			ret = -EIO;
		else
			ret = apple_rtkit_send_message(cam->rtk, endpoint, address, NULL, true);
		spin_unlock_irqrestore(&cam->service_lock, flags);
	}
	if (ret) {
		WRITE_ONCE(cam->query_stalled, true);
		cam_failf(cam, "IPC send stalled (%s: %d)", name, ret);
	}
	return ret;
}

/* Nonzero kinds can contain binary payloads. Bound both formats to the slot. */
void cam_print_other(struct cam_device *cam, const struct cam_print_record *record,
		     unsigned int back)
{
	char text[2 * sizeof(record->text) + 1];
	size_t size = strnlen(record->text, sizeof(record->text)), length = size, i;
	const char *encoding = "";

	/* Firmware console text ends in newlines, as kind-0 records show. */
	while (length && record->text[length - 1] == '\n')
		length--;
	for (i = 0; i < length; i++)
		if ((u8)record->text[i] < ' ' || (u8)record->text[i] > '~')
			break;
	if (size && i == length) {
		memcpy(text, record->text, length);
	} else {
		encoding = "hex=";
		for (i = 0; i < sizeof(record->text); i++)
			scnprintf(text + 2 * i, 3, "%02x", (u8)record->text[i]);
		length = 2 * sizeof(record->text);
	}
	text[length] = 0;
	if (back)
		dev_info(cam->dev, "ISP[-%u]: kind=%u seq=%u %s%s\n", back,
			 le16_to_cpu(record->kind), le16_to_cpu(record->sequence), encoding, text);
	else
		dev_info(cam->dev, "ISP: kind=%u seq=%u %s%s\n",
			 le16_to_cpu(record->kind), le16_to_cpu(record->sequence), encoding, text);
}

static bool cam_print_poll(struct cam_device *cam)
{
	/* Default-on and possibly per frame: at most ten lines a minute. */
	static DEFINE_RATELIMIT_STATE(other, 60 * HZ, 10);
	void *ring = cam_pointer(cam, CAM_PRINT_BASE, CAM_PRINT_SIZE);
	struct cam_print_record record;
	u32 producer, cursor;
	unsigned int budget = 128;

	lockdep_assert_held(&cam->fw_lock);
	if (!ring || cam->print_failed || READ_ONCE(cam->transport_dead))
		return false;
	producer = le32_to_cpu(READ_ONCE(*(__le32 *)ring));
	if (!producer)
		return false;
	cursor = cam->print_cursor;
	if (!cam_print_cursor_valid(producer) || !cam_print_cursor_valid(cursor)) {
		cam->print_failed = true;
		cam_fail(cam, "invalid firmware print-ring cursor");
		return false;
	}
	dma_rmb();
	while (cursor != producer && budget--) {
		memcpy(&record, ring + cursor, sizeof(record));
		cam_output_record(&cam->session.output, &record);
		if (!le16_to_cpu(record.kind)) {
			if (cam_fw_log)
				dev_info(cam->dev, "ISP: %.*s\n", (int)sizeof(record.text), record.text);
			else
				dev_dbg(cam->dev, "ISP: %.*s\n", (int)sizeof(record.text), record.text);
			if (strnstr(record.text, "eris SMP", sizeof(record.text)) && ++cam->banners > 1) {
				cam_transport_fail(cam, "firmware restarted");
				return false;
			}
			if (strnstr(record.text, "[ISP] CTRL READY", sizeof(record.text)))
				complete(&cam->ready);
		} else if (__ratelimit(&other)) {
			cam_print_other(cam, &record, 0);
		}
		cursor = cam_print_next(cursor);
	}
	cam->print_cursor = cursor;
	dma_mb();
	WRITE_ONCE(*((__le32 *)ring + 1), cpu_to_le32(cursor));
	dma_wmb();
	return cursor != producer;
}

void cam_print_drain(struct cam_device *cam)
{
	unsigned int i;

	for (i = 0; i < DIV_ROUND_UP(CAM_PRINT_END, 128 * CAM_PRINT_RECORD_SIZE); i++)
		if (!cam_print_poll(cam))
			break;
}

void cam_delay(struct cam_device *cam, unsigned int duration_ms)
{
	unsigned int waited;

	for (waited = 0; waited < duration_ms; waited += 5) {
		cam_print_poll(cam);
		if (READ_ONCE(cam->transport_dead))
			break;
		usleep_range(5000, 6000);
	}
	cam_print_poll(cam);
}

bool cam_wait(struct cam_device *cam, struct completion *done,
	      unsigned int timeout_ms, bool stopping)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	bool completed;

	do {
		cam_print_poll(cam);
		if (READ_ONCE(cam->transport_dead) || (!stopping && cam_dead(cam)))
			return false;
		completed = wait_for_completion_timeout(done, msecs_to_jiffies(5)) != 0;
		cam_print_poll(cam);
		if (completed)
			return !READ_ONCE(cam->transport_dead) && (stopping || !cam_dead(cam));
	} while (time_before(jiffies, deadline));
	return false;
}

/* Name the command before its reply can rewrite the command buffer. */
static void cam_command_name(const u8 *command, u8 endpoint, char *name, size_t size)
{
	if (endpoint == 0x24)
		scnprintf(name, size, "buffer list n=%llu", get_unaligned_le64(command + 8));
	else
		scnprintf(name, size, "%s %#x", endpoint == 0x23 ? "query" : "command",
			  get_unaligned_le16(command + 4));
}

int cam_message(struct cam_device *cam, u8 endpoint, size_t size, u64 argument)
{
	struct cam_record *record = cam_pointer(cam, CAM_QUERY_RECORD, sizeof(*record));
	const u8 *command;
	char name[32], reason[64];
	u64 start, status;
	int ret;

	lockdep_assert_held(&cam->fw_lock);
	if (!cam_can_send(cam))
		return -EIO;
	if (!record || !size || size > CAM_QUERY_CAPACITY ||
	    !cam_pointer(cam, CAM_QUERY_COMMAND, size)) {
		cam_fail(cam, "invalid command range");
		return -ERANGE;
	}
	command = cam_pointer(cam, CAM_QUERY_COMMAND, size);
	cam_command_name(command, endpoint, name, sizeof(name));
	dev_dbg(cam->dev, "%s send: ep=%#x size=%#zx arg=%#llx\n", name, endpoint, size, argument);
	memset(record, 0, sizeof(*record));
	record->arg[0] = cpu_to_le64(CAM_QUERY_COMMAND);
	record->arg[1] = cpu_to_le64(size);
	record->arg[2] = cpu_to_le64(argument);
	reinit_completion(&cam->reply);
	WRITE_ONCE(cam->pending_endpoint, endpoint);
	WRITE_ONCE(cam->query_pending, true);
	dma_wmb();
	start = ktime_get_ns();
	ret = cam_send(cam, endpoint, CAM_QUERY_RECORD, name);
	/* A DEAD session still completes its ordered stop on a responsive channel. */
	if (!ret && !cam_wait(cam, &cam->reply, 2000, true))
		ret = -ETIMEDOUT;
	WRITE_ONCE(cam->query_pending, false);
	if (ret) {
		WRITE_ONCE(cam->query_stalled, true);
		scnprintf(reason, sizeof(reason), "IPC %s timed out", name);
		cam_fail(cam, reason);
	}
	dma_rmb();
	status = endpoint == 0x24 ? le64_to_cpu(record->arg[2]) : get_unaligned_le16(command + 6);
	dev_dbg(cam->dev, "%s reply: ep=%#x result=%d status=%#llx elapsed=%lluus\n",
		name, endpoint, ret, status, (ktime_get_ns() - start) / NSEC_PER_USEC);
	if (ret)
		cam_dead_dump(cam);
	return ret;
}

int cam_exchange(struct cam_device *cam, u8 endpoint, size_t insize, size_t outsize)
{
	if (outsize > CAM_QUERY_CAPACITY ||
	    !cam_pointer(cam, CAM_QUERY_COMMAND, max(insize, outsize))) {
		cam_fail(cam, "invalid command payload range");
		return -ERANGE;
	}
	return cam_message(cam, endpoint, insize, outsize);
}

static int cam_malloc(struct cam_device *cam, struct cam_record *record)
{
	u64 address = le64_to_cpu(record->arg[0]);
	u64 size = le64_to_cpu(record->arg[1]);
	u64 tag = le64_to_cpu(record->arg[2]);
	unsigned long flags;
	size_t offset;
	bool ok;

	spin_lock_irqsave(&cam->service_lock, flags);
	if (!address) {
		ok = !cam_dead(cam) && cam_arena_take(&cam->arena, size, tag, &offset);
		if (ok) {
			address = cam->arena_base + offset;
			if (tag == 0x4d495343 && size == 0xc000 && !cam->bt_iova) {
				cam->bt_iova = address;
				cam->bt_size = size;
			}
		}
	} else {
		ok = address >= cam->arena_base &&
			cam_arena_release(&cam->arena, address - cam->arena_base, size, tag);
	}
	cam_publish_status(cam);
	spin_unlock_irqrestore(&cam->service_lock, flags);
	if (!ok) {
		if (!le64_to_cpu(record->arg[0])) {
			char reason[100];

			scnprintf(reason, sizeof(reason), "arena exhausted after %u sessions", cam->sessions);
			cam_fail(cam, reason);
		} else {
			cam_fail(cam, "invalid arena free");
		}
		return -EPROTO;
	}
	dev_dbg(cam->dev, "shared address=%#llx size=%#llx tag=%#llx\n", address, size, tag);
	record->arg[0] = cpu_to_le64(address);
	record->arg[1] = 0;
	record->arg[2] = 0;
	return 0;
}

static int cam_buffer_return(struct cam_device *cam, struct cam_record *record,
			     struct cam_returned *batch)
{
	u64 raw = le64_to_cpu(record->arg[0]);
	u64 address = cam_ipc_iova(raw & ~3ULL);
	u64 size = le64_to_cpu(record->arg[1]);
	const struct cam_buffer_list *list;
	struct cam_session *session = &cam->session;
	unsigned long flags;
	size_t offset;
	unsigned int i;
	bool ok;

	if (!cam->bt_iova || size < sizeof(*list) || size > cam->bt_size ||
	    !d_range_offset(address, cam->bt_iova, cam->bt_size, size, &offset))
		return -ERANGE;
	list = cam_pointer(cam, address, size);
	if (!list)
		return -ERANGE;
	spin_lock_irqsave(&cam->service_lock, flags);
	ok = cam_validate_returns(&cam->geometry, cam->capture_base, list, size,
				  session->metadata_issued, session->capture_issued,
				  &session->returned, batch);
	if (!ok || session->order_count + batch->count > CAM_FRAME_COUNT) {
		spin_unlock_irqrestore(&cam->service_lock, flags);
		return -EPROTO;
	}
	session->returned.metadata |= batch->metadata;
	session->returned.capture |= batch->capture;
	session->returned.images |= batch->images;
	if (le64_to_cpu(record->arg[2]) == CAM_BUFFER_RENDER)
		session->completed |= batch->images;
	for (i = 0; i < batch->count; i++) {
		unsigned int slot = batch->order[i];

		session->order[session->order_count++] = slot;
		session->timestamps[slot] = ktime_get_ns();
		if (hweight32(batch->metadata) == 1 && batch->count == 1)
			session->frame_meta[slot] = __ffs(batch->metadata);
	}
	session->buffer_returns += batch->total;
	spin_unlock_irqrestore(&cam->service_lock, flags);
	record->arg[0] = cpu_to_le64(raw);
	record->arg[1] = 0;
	record->arg[2] = cpu_to_le64(CAM_BUFFER_ACK);
	return 0;
}

static void cam_terminal(struct cam_device *cam, const struct cam_record *record)
{
	u64 address = le64_to_cpu(record->arg[0]) & ~3ULL;
	u64 size = min_t(u64, le64_to_cpu(record->arg[1]), 256);
	const char *line = cam_pointer(cam, address, size);

	/* Held slots are never acknowledged, as in Stage D. */
	cam->terminal_held++;
	dev_info(cam->dev, "held TERMINAL record %u text=%#llx/%#llx\n",
		 cam->terminal_held, address, size);
	if (line && strnstr(line, "[ISP] CTRL READY", size))
		complete(&cam->ready);
	if (cam->terminal_held >= cam_channel_expected[0].count)
		cam_fail(cam, "TERMINAL slot cap reached");
}

void cam_receive(void *cookie, u8 endpoint, u64 message)
{
	struct cam_device *cam = cookie;
	u64 address = message & 0x00ffffffffffffffULL;
	const struct cam_channel_expected *channel;
	struct cam_record record, *shared;
	struct cam_returned batch = { 0 };
	unsigned long flags;
	size_t offset;
	unsigned int index;
	int ret;

	/* Pair with bootstrap publication of the initialized service. */
	if (!smp_load_acquire(&cam->service_active) || READ_ONCE(cam->transport_dead))
		return;
	if (endpoint == READ_ONCE(cam->pending_endpoint) && address == CAM_QUERY_RECORD &&
	    READ_ONCE(cam->query_pending)) {
		complete(&cam->reply);
		return;
	}
	if (endpoint == 0x20)
		index = 0;
	else if (endpoint == 0x25)
		index = 4;
	else if (endpoint == 0x26)
		index = 5;
	else
		goto reject;
	channel = &cam_channel_expected[index];
	if (!d_range_offset(address, CAM_IPC_BASE + channel->offset,
			    channel->count * CAM_SLOT_SIZE, sizeof(record), &offset) ||
	    offset % CAM_SLOT_SIZE)
		goto reject;
	shared = cam_pointer(cam, address, sizeof(record));
	if (!shared)
		goto reject;
	dma_rmb();
	memcpy(&record, shared, sizeof(record));
	if (endpoint == 0x26)
		ret = cam_malloc(cam, &record);
	else if (endpoint == 0x25)
		ret = cam_buffer_return(cam, &record, &batch);
	else {
		if (cam->terminal_held >= channel->count)
			goto reject;
		cam_terminal(cam, &record);
		return;
	}
	if (ret)
		goto reject;
	memcpy(shared->arg, record.arg, sizeof(record.arg));
	dma_wmb();
	ret = cam_send(cam, endpoint, address, endpoint == 0x26 ? "malloc ACK" : "return ACK");
	if (ret)
		return;
	if (endpoint == 0x25) {
		/* Publish acknowledgement separately: a stale completion must never
		 * expose a later return whose acknowledgement is still in flight.
		 */
		spin_lock_irqsave(&cam->service_lock, flags);
		cam->session.acked_images |= batch.images;
		cam->session.acked_meta |= batch.metadata;
		cam->session.acked_capture |= batch.capture;
		spin_unlock_irqrestore(&cam->service_lock, flags);
		complete(&cam->frames);
	}
	return;
reject:
	cam_fail(cam, "invalid service record");
}

static int cam_shmem_setup(void *cookie, struct apple_rtkit_shmem *buffer)
{
	struct cam_device *cam = cookie;
	size_t offset, size;

	if (!buffer->size || buffer->size > CAM_SHMEM_SIZE)
		goto reject;
	if (buffer->iova) {
		if (d_range_offset(buffer->iova, CAM_VM_BASE + CAM_CODE_SIZE,
				   CAM_DATA_SIZE, buffer->size, &offset))
			buffer->iomem = cam->data + offset;
		else if (d_range_offset(buffer->iova, CAM_VM_BASE + CAM_FW_END,
					CAM_HEAP_END - CAM_FW_END, buffer->size, &offset))
			buffer->buffer = cam->ram + offset;
		else
			goto reject;
		buffer->is_mapped = true;
	} else {
		size = PAGE_ALIGN(buffer->size);
		if (size > CAM_SHMEM_SIZE - cam->shmem_used)
			goto reject;
		buffer->iova = CAM_VM_BASE + CAM_HEAP_END + cam->shmem_used;
		buffer->buffer = cam->ram + CAM_HEAP_END - CAM_FW_END + cam->shmem_used;
		cam->shmem_used += size;
	}
	return 0;
reject:
	cam_fail(cam, "unsupported RTKit shared-memory range");
	return -ERANGE;
}

static void cam_shmem_destroy(void *cookie, struct apple_rtkit_shmem *buffer)
{
	/* All pages remain held until power-off after RUN. */
}

static void cam_crashed(void *cookie, const void *crashlog, size_t size)
{
	struct cam_device *cam = cookie;

	cam_transport_fail(cam, "RTKit crash");
}

const struct apple_rtkit_ops cam_rtkit_ops = {
	.shmem_setup = cam_shmem_setup,
	.shmem_destroy = cam_shmem_destroy,
	.crashed = cam_crashed,
	.recv_message = cam_receive,
};

static void cam_reclaim(struct cam_device *cam)
{
	unsigned long flags;
	size_t cursor = 0, offset, size;
	bool found;

	spin_lock_irqsave(&cam->service_lock, flags);
	if (cam_dead(cam)) {
		spin_unlock_irqrestore(&cam->service_lock, flags);
		return;
	}
	cam_arena_begin_reclaim(&cam->arena);
	spin_unlock_irqrestore(&cam->service_lock, flags);
	for (;;) {
		spin_lock_irqsave(&cam->service_lock, flags);
		found = !cam_dead(cam) &&
			cam_arena_reclaim_range(&cam->arena, cursor, &offset, &size);
		spin_unlock_irqrestore(&cam->service_lock, flags);
		if (!found)
			break;
		memset(cam_pointer(cam, cam->arena_base + offset, size), 0, size);
		cursor = offset + size;
		cond_resched();
	}
	dma_wmb();
	spin_lock_irqsave(&cam->service_lock, flags);
	if (!cam_dead(cam))
		cam_arena_finish_reclaim(&cam->arena);
	cam_publish_status(cam);
	spin_unlock_irqrestore(&cam->service_lock, flags);
}

static bool cam_all_back(struct cam_device *cam)
{
	struct cam_session *s = &cam->session;
	unsigned long flags;
	bool back;

	spin_lock_irqsave(&cam->service_lock, flags);
	back = (!s->metadata_issued || s->acked_meta == CAM_ALL_META) &&
		(!s->capture_issued || (s->acked_capture == CAM_ALL_META &&
				       s->acked_images == CAM_ALL_IMAGES));
	spin_unlock_irqrestore(&cam->service_lock, flags);
	return back;
}

void cam_session_stop(struct cam_device *cam)
{
	struct cam_session *s = &cam->session;
	struct cam_arena_stats stats;
	unsigned long flags, deadline;
	u64 elapsed, fps, copy_us;
	int ret;

	lockdep_assert_held(&cam->fw_lock);
	if (!s->active || s->stopped)
		return;
	/* Mark first so STREAMOFF after shutdown cannot issue a second stop. */
	s->stopped = true;
	cam_dead_dump(cam);
	if (s->channel_issued && cam_can_send(cam)) {
		ret = cam_channel_input(cam, 0x101, 0, false);
		if (ret)
			cam_fail(cam, "CH_STOP failed");
		if (cam_can_send(cam)) {
			ret = cam_channel_input(cam, 0x104, 0, false);
			if (ret)
				cam_fail(cam, "channel buffer return failed");
		}
	}
	if (s->metadata_issued && cam_can_send(cam)) {
		deadline = jiffies + msecs_to_jiffies(500);
		while (!cam_all_back(cam) && time_before(jiffies, deadline) && cam_can_send(cam))
			cam_delay(cam, 5);
		if (!cam_all_back(cam))
			cam_fail(cam, "buffers missing after 0x104");
	}
	if (s->control_issued && cam_can_send(cam)) {
		ret = cam_control(cam, false);
		if (ret)
			cam_fail(cam, "control STOP failed");
	}
	if (cam_can_send(cam)) {
		ret = cam_poweroff_sensor(cam);
		if (ret)
			cam_fail(cam, "POWER_DEVICE_OFF failed");
	}
	if (!cam_dead(cam) && !READ_ONCE(cam->shutting_down)) {
		cam_reclaim(cam);
		cam_guard_audit(cam);
	}
	cam_dead_dump(cam);
	cam->last_stop_ns = ktime_get_ns();
	cam_experiment_end(cam);
	spin_lock_irqsave(&cam->service_lock, flags);
	stats = cam_arena_stats(&cam->arena);
	cam_publish_status(cam);
	spin_unlock_irqrestore(&cam->service_lock, flags);
	elapsed = max_t(u64, (ktime_get_ns() - s->start_ns) / NSEC_PER_MSEC, 1);
	fps = (u64)s->sequence * 10000 / elapsed;
	copy_us = s->delivered ? s->copy_ns / s->delivered / NSEC_PER_USEC : 0;
	dev_info(cam->dev,
		 "session %u stop: frames=%u delivered=%u dropped=%u fps=%llu.%llu requeues=%u copy_avg=%lluus arena live=%zu pending=%zu peak=%zu entries=%u\n",
		 cam->sessions, s->sequence, s->delivered, s->dropped, fps / 10, fps % 10,
		 s->requeues, copy_us, stats.live, stats.pending, stats.peak, stats.entries);
	dev_info(cam->dev, "session %u pacing: gaps=%u avg=%lluus max=%lluus\n",
		 cam->sessions, s->pacing_gaps,
		 s->pacing_gaps ? s->pacing_gap_ns / s->pacing_gaps / NSEC_PER_USEC : 0,
		 s->pacing_max_gap_ns / NSEC_PER_USEC);
}

static bool cam_batch(struct cam_device *cam)
{
	struct cam_session *s = &cam->session;
	unsigned long deadline = jiffies + msecs_to_jiffies(1000), flags, now;
	u8 order[CAM_FRAME_COUNT], ready[CAM_FRAME_COUNT];
	u64 timestamps[CAM_FRAME_COUNT];
	u32 completed, acked, delivered = 0;
	unsigned int order_count, count, i;

	for (;;) {
		if (cam_dead(cam))
			return false;
		spin_lock_irqsave(&cam->service_lock, flags);
		order_count = s->order_count;
		memcpy(order, s->order, sizeof(order));
		memcpy(timestamps, s->timestamps, sizeof(timestamps));
		completed = s->completed;
		acked = s->acked_images;
		spin_unlock_irqrestore(&cam->service_lock, flags);
		count = cam_ready_slots(order, order_count, completed, acked, &delivered, ready);
		for (i = 0; i < count; i++) {
			if (cam_dead(cam))
				return false;
			/* Each slot stays owned by the host until the batch requeue. */
			cam_deliver(cam, ready[i], timestamps[ready[i]]);
		}
		if (cam_dead(cam))
			return false;
		if (acked == CAM_ALL_IMAGES)
			return true;
		now = jiffies;
		if (time_after_eq(now, deadline) ||
		    !cam_wait(cam, &cam->frames,
			      max_t(unsigned int, jiffies_to_msecs(deadline - now), 1), false)) {
			cam_fail(cam, "batch timeout (1 second)");
			return false;
		}
	}
}

static int cam_stream_thread(void *data)
{
	struct cam_device *cam = data;
	struct cam_session *s = &cam->session;
	unsigned long flags;
	u32 meta;
	int ret;

	complete(&cam->stream_started);
	mutex_lock(&cam->fw_lock);
	while (cam_batch(cam)) {
		cam_delay(cam, CAM_STREAM_SETTLE_MS);
		if (READ_ONCE(s->stop_requested) || kthread_should_stop() || cam_dead(cam))
			break;
		spin_lock_irqsave(&cam->service_lock, flags);
		meta = s->acked_meta;
		spin_unlock_irqrestore(&cam->service_lock, flags);
		if (hweight32(meta) < CAM_META_COUNT - CAM_STREAM_META_LOW)
			meta = 0;
		ret = cam_requeue(cam, meta, CAM_ALL_IMAGES);
		if (ret) {
			cam_fail(cam, "buffer requeue failed");
			break;
		}
		/* A return may latch DEAD while the requeue still gets its reply. */
		if (cam_framing_requeue(cam) || cam_dead(cam))
			break;
	}
	cam_session_stop(cam);
	/* Keep task_struct alive until STREAMOFF/shutdown joins this task. */
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	mutex_unlock(&cam->fw_lock);
	return 0;
}

static int cam_session_setup(struct cam_device *cam)
{
	struct cam_session *s = &cam->session;
	int ret = 0;

	/* Every firmware start runs session 1's sequence, 0x3206 included. */
	if (cam->fw_sessions == 1)
		ret = cam_platform(cam);
	if (ret || cam_dead(cam))
		return -EIO;
	s->control_issued = true;
	ret = cam_control(cam, true);
	if (!ret && !cam_dead(cam))
		ret = cam_sensor_info(cam);
	if (!ret && !cam_dead(cam))
		ret = cam_experiment_round(cam);
	if (!ret && !cam_dead(cam))
		ret = cam_select(cam);
	if (!ret && !cam_dead(cam))
		ret = cam_current(cam);
	if (!ret && !cam_dead(cam))
		ret = cam_configure_buffers(cam);
	if (!ret && !cam_dead(cam))
		ret = cam_metadata(cam);
	if (!ret && !cam_dead(cam))
		ret = cam_channel_input(cam, 0x110, 0, false);
	if (!ret && !cam_dead(cam)) {
		s->channel_issued = true;
		ret = cam_channel_input(cam, 0x100, 0, false);
	}
	if (!ret && !cam_dead(cam))
		ret = cam_experiment_load(cam);
	if (!ret && !cam_dead(cam))
		ret = cam_framing_start(cam);
	if (!ret && !cam_dead(cam))
		ret = cam_fps_start(cam);
	if (!ret && !cam_dead(cam))
		ret = cam_output_verify(cam, false);
	if (!ret && !cam_dead(cam))
		ret = cam_capture(cam);
	if (!ret && !cam_dead(cam))
		ret = cam_output_verify(cam, true);
	return cam_dead(cam) ? -EIO : ret;
}

/* RTKit's power acknowledgements wait interruptibly and kthread_run waits
 * killably. The task closing or starting a stream may have a signal
 * pending, so its firmware steps run here, where no signal is delivered.
 * The caller holds video_lock, so one step runs at a time; it must not hold
 * fw_lock, which the step takes. Every wait inside a step is bounded.
 */
int cam_lifecycle(struct cam_device *cam, int (*step)(struct cam_device *cam))
{
	lockdep_assert_held(&cam->video_lock);
	lockdep_assert_not_held(&cam->fw_lock);
	cam->lifecycle_step = step;
	WARN_ON(!queue_work(cam->lifecycle, &cam->lifecycle_work));
	flush_work(&cam->lifecycle_work);
	return cam->lifecycle_result;
}

void cam_lifecycle_work(struct work_struct *work)
{
	struct cam_device *cam = container_of(work, struct cam_device, lifecycle_work);

	mutex_lock(&cam->fw_lock);
	cam->lifecycle_result = cam->lifecycle_step(cam);
	mutex_unlock(&cam->fw_lock);
}

/* A lifecycle step: the stream thread is created from the worker. */
int cam_session_start(struct cam_device *cam)
{
	unsigned long flags;
	int ret;

	lockdep_assert_held(&cam->fw_lock);
	spin_lock_irqsave(&cam->service_lock, flags);
	if (cam_dead(cam)) {
		spin_unlock_irqrestore(&cam->service_lock, flags);
		cam_dead_dump(cam);
		return -EIO;
	}
	cam->session = (struct cam_session) { .active = true, .start_ns = ktime_get_ns() };
	reinit_completion(&cam->frames);
	cam->sessions++;
	cam->fw_sessions++;
	cam_publish_status(cam);
	spin_unlock_irqrestore(&cam->service_lock, flags);
	cam_experiment_begin(cam);
	if (cam->last_stop_ns)
		dev_info(cam->dev, "session %u begin: %llu ms after session %u stopped\n",
			 cam->sessions, (cam->session.start_ns - cam->last_stop_ns) / NSEC_PER_MSEC,
			 cam->sessions - 1);
	else
		dev_info(cam->dev, "session %u begin: first session of this firmware boot\n",
			 cam->sessions);
	cam_print_drain(cam);
	ret = -EIO;
	if (!cam_dead(cam)) {
		memset(cam_pointer(cam, cam->capture_base, cam->geometry.capture_size), 0,
		       cam->geometry.capture_size);
		dma_wmb();
		ret = cam_session_setup(cam);
	}
	if (!ret) {
		reinit_completion(&cam->stream_started);
		cam->thread = kthread_run(cam_stream_thread, cam, "j713-camera");
		if (IS_ERR(cam->thread)) {
			ret = PTR_ERR(cam->thread);
			cam->thread = NULL;
		}
	}
	if (ret) {
		if (!cam->session.output_rejected)
			cam_fail(cam, "session start failed");
		cam_session_stop(cam);
	} else {
		dev_info(cam->dev, "session %u start: output=%ux%u crop_y=%u\n",
			 cam->sessions, cam->geometry.width, cam->geometry.height, cam->geometry.crop_y);
	}
	return ret ? -EIO : 0;
}

void cam_stream_join(struct cam_device *cam)
{
	lockdep_assert_held(&cam->video_lock);
	WRITE_ONCE(cam->session.stop_requested, true);
	if (cam->thread) {
		kthread_stop(cam->thread);
		cam->thread = NULL;
	}
	mutex_lock(&cam->fw_lock);
	cam_session_stop(cam);
	mutex_unlock(&cam->fw_lock);
}

/* RTKit has been freed and the CPU power-stopped. No old worker survives.
 * Keep the claim, mappings, lifetime counters and completion wait queues.
 */
void cam_reset_memory(struct cam_device *cam)
{
	unsigned long flags;
	size_t high;

	lockdep_assert_held(&cam->fw_lock);
	spin_lock_irqsave(&cam->service_lock, flags);
	high = cam->arena.high;
	cam_arena_reset(&cam->arena);
	cam->bt_iova = 0;
	cam->bt_size = 0;
	cam->session = (struct cam_session) { };
	cam_publish_status(cam);
	spin_unlock_irqrestore(&cam->service_lock, flags);
	memset(cam->ram, 0, cam->ram_size);
	memset(cam_pointer(cam, cam->window_base, CAM_EXTRA_SIZE), 0, CAM_EXTRA_SIZE);
	if (high)
		memset(cam_pointer(cam, cam->arena_base, high), 0, high);
	memset(cam_pointer(cam, cam->capture_base, cam->capture_span), 0, cam->capture_span);
	dma_wmb();
	cam->shmem_used = 0;
	cam->print_cursor = CAM_PRINT_RECORD_SIZE;
	cam->banners = 0;
	cam->terminal_held = 0;
	cam->sensor_version = 0;
	cam->presets = 0;
	cam->word_68 = 0;
	cam->word_78 = 0;
	WRITE_ONCE(cam->service_active, false);
	WRITE_ONCE(cam->query_pending, false);
	WRITE_ONCE(cam->query_stalled, false);
	WRITE_ONCE(cam->pending_endpoint, 0);
	WRITE_ONCE(cam->transport_dead, false);
	cam->print_failed = false;
	cam->dead_dumped = false;
	cam->fw_sessions = 0;
	reinit_completion(&cam->reply);
	reinit_completion(&cam->ready);
	reinit_completion(&cam->frames);
	reinit_completion(&cam->stream_started);
	dev_info(cam->dev, "re-arm: memory reset (translated %zu, arena high %zu bytes)\n",
		 cam->ram_size, high);
}

void cam_idle_work(struct work_struct *work)
{
	struct cam_device *cam = container_of(to_delayed_work(work), struct cam_device, idle_work);

	if (READ_ONCE(cam->shutting_down))
		return;
	/* Its last run after an idle-time DEAD dumps the print ring. */
	if (mutex_trylock(&cam->fw_lock)) {
		/* A power-stop or re-arm may have finished before we got the lock. */
		if (READ_ONCE(cam->state) == CAM_IDLE && cam->firmware_running &&
		    !READ_ONCE(cam->shutting_down))
			cam_print_drain(cam);
		cam_dead_dump(cam);
		mutex_unlock(&cam->fw_lock);
	}
	/* A power-stopped firmware has nothing to drain until the next start. */
	if (!READ_ONCE(cam->shutting_down) && !cam_dead(cam) &&
	    READ_ONCE(cam->firmware_running))
		schedule_delayed_work(&cam->idle_work, HZ);
}
