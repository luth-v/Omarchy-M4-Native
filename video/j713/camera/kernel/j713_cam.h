/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef J713_CAM_H
#define J713_CAM_H

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/soc/apple/mailbox.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "j713_cam_wire.h"

#define CAM_VM_BASE 0x10000000000ULL
#define CAM_CODE_SIZE 0xca4000UL
#define CAM_DATA_SIZE 0x748000UL
#define CAM_FW_END (CAM_CODE_SIZE + CAM_DATA_SIZE)
#define CAM_HEAP_END 0x2000000UL
#define CAM_SHMEM_SIZE SZ_16M
/* The 82f9fd25 claim; probe adds the 1080p tail only where it also fits. */
#define CAM_IDENTITY_SIZE (CAM_EXTRA_SIZE + CAM_ARENA_SIZE + CAM_CAPTURE_SIZE)
#define CAM_IDENTITY_MAX (CAM_EXTRA_SIZE + CAM_ARENA_SIZE + CAM_CAPTURE_MAX)
#define CAM_IDENTITY_SPAN(cam) (CAM_EXTRA_SIZE + CAM_ARENA_SIZE + (cam)->capture_span)
#define CAM_SHARED_VA_END 0xf6000000ULL
#define CAM_ALL_META ((1U << CAM_META_COUNT) - 1)
#define CAM_ALL_IMAGES ((1U << CAM_FRAME_COUNT) - 1)
#define CAM_GUARD_PATTERN(index) (0xEA7E17A2C0DE0000ULL | (u64)(index))

enum cam_state {
	CAM_PROBED,
	CAM_IDLE,
	CAM_STREAMING,
	CAM_DEAD,
};

struct cam_experiment_config {
	unsigned int framing, colour, fps;
};

struct cam_experiment {
	struct cam_experiment_config config;
	const struct firmware *blob;
	bool enabled, colour_active;
	bool framing_enabled, framing_pending;
	u32 framing_delivered;
	unsigned int frame_rate;
};

struct cam_session {
	struct cam_experiment experiment;
	struct cam_output_evidence output;
	bool output_rejected;
	struct cam_returned returned;
	u32 completed;
	u32 acked_images, acked_meta, acked_capture;
	u8 frame_meta[CAM_FRAME_COUNT], order[CAM_FRAME_COUNT];
	unsigned int order_count;
	u64 timestamps[CAM_FRAME_COUNT];
	bool metadata_issued, metadata_submitted, capture_issued, capture_submitted;
	bool control_issued, channel_issued, active, stopped, stop_requested;
	struct cam_white_balance wb;
	u32 sequence, delivered, dropped, requeues, buffer_returns;
	u32 pacing_gaps;
	u64 start_ns, copy_ns;
	u64 last_delivery_ns, pacing_gap_ns, pacing_max_gap_ns;
};

struct cam_buffer_vb2 {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct cam_device {
	struct device *dev;
	struct v4l2_device v4l2;
	struct video_device video;
	struct vb2_queue queue;
	struct v4l2_ctrl_handler controls;
	/* video_lock -> fw_lock -> service_lock OR buffers_lock. The receive
	 * worker takes only service_lock, never either sleeping mutex.
	 * Boot, session start, power-stop and shutdown sleep run on the
	 * lifecycle worker, which takes fw_lock and never video_lock. Its
	 * caller holds video_lock but not fw_lock, and has joined the stream
	 * thread, so only a brief idle drain can delay the worker's fw_lock.
	 */
	struct mutex video_lock, fw_lock;
	spinlock_t service_lock, buffers_lock;
	struct list_head buffers;
	struct task_struct *thread;
	struct delayed_work idle_work;
	struct workqueue_struct *lifecycle;
	struct work_struct lifecycle_work;
	int (*lifecycle_step)(struct cam_device *cam);
	int lifecycle_result;
	enum cam_state state;
	char dead_reason[160];
	/* started: RUN was written once this load (pinned). running: from RUN
	 * until RTKit sleeps before a power-stop; nothing is sent while clear.
	 */
	bool firmware_started, firmware_running, transport_dead, shutting_down, queue_ready;
	bool auto_white_balance, alias_fix;
	unsigned int sessions, fw_starts, fw_sessions;
	unsigned int frame_rate;
	struct cam_geometry geometry, format;
	u32 out_height;
	struct cam_session session;
	struct cam_arena arena;
	struct apple_rtkit *rtk;
	struct apple_mbox *mbox;
	struct iommu_domain *original, *domain;
	bool attached, backing_unsafe;
	void __iomem *cpu, *rvbar, *data, *gpio, *cpu_page;
	u32 control;
	void *ram, *identity;
	dma_addr_t ram_dma;
	size_t ram_size, map_end, mapped, identity_mapped, shmem_used;
	unsigned int mmio_mapped;
	struct page *claim_pages;
	unsigned long claim_count;
	/* CAM_CAPTURE_SIZE, or CAM_CAPTURE_MAX when 1080p fits the claim. */
	size_t capture_span;
	u64 window_base, arena_base, capture_base, bt_iova;
	size_t bt_size;
	/* Wait queues have device lifetime; session reset only clears data. */
	struct completion reply, ready, frames, stream_started;
	bool service_active, query_pending, query_stalled, print_failed, dead_dumped;
	u8 pending_endpoint;
	u32 print_cursor, banners, terminal_held;
	u64 last_stop_ns, poweroff_ns;
	u32 sensor_version, presets, word_68, word_78;
};

extern const struct apple_rtkit_ops cam_rtkit_ops;
extern const bool cam_fw_log, cam_fw_restart;
extern int cam_setfile_result, cam_lenscal_result;

static inline bool cam_dead(const struct cam_device *cam)
{
	return READ_ONCE(cam->state) == CAM_DEAD;
}

static inline bool cam_can_send(const struct cam_device *cam)
{
	return READ_ONCE(cam->firmware_running) && !READ_ONCE(cam->transport_dead) &&
		!READ_ONCE(cam->query_stalled);
}

void cam_publish_status(struct cam_device *cam);
void cam_fail(struct cam_device *cam, const char *reason);
__printf(2, 3) void cam_failf(struct cam_device *cam, const char *format, ...);
void cam_set_state(struct cam_device *cam, enum cam_state state);
int cam_lifecycle(struct cam_device *cam, int (*step)(struct cam_device *cam));
void cam_lifecycle_work(struct work_struct *work);
int cam_boot(struct cam_device *cam);
int cam_fw_stop(struct cam_device *cam);
void cam_reset_memory(struct cam_device *cam);
void cam_guard_audit(struct cam_device *cam);
bool cam_memory_release(struct cam_device *cam);
void *cam_pointer(struct cam_device *cam, u64 address, size_t size);
void cam_receive(void *cookie, u8 endpoint, u64 message);
void cam_print_drain(struct cam_device *cam);
void cam_print_other(struct cam_device *cam, const struct cam_print_record *record,
		     unsigned int back);
void cam_dead_dump(struct cam_device *cam);
void cam_delay(struct cam_device *cam, unsigned int duration_ms);
bool cam_wait(struct cam_device *cam, struct completion *done,
	      unsigned int timeout_ms, bool stopping);
int cam_message(struct cam_device *cam, u8 endpoint, size_t size, u64 argument);
int cam_exchange(struct cam_device *cam, u8 endpoint, size_t insize, size_t outsize);
int cam_boot_queries(struct cam_device *cam);
int cam_platform(struct cam_device *cam);
int cam_control(struct cam_device *cam, bool start);
int cam_sensor_info(struct cam_device *cam);
int cam_lenscal_get(struct cam_device *cam);
int cam_lenscal_set(struct cam_device *cam, unsigned int number);
int cam_setfile_stage(struct cam_device *cam);
int cam_setfile_load(struct cam_device *cam);
int cam_framing_crop(struct cam_device *cam, const char *when);
int cam_framing_zoom(struct cam_device *cam, const char *when, u16 zoom[2]);
int cam_framing_pin(struct cam_device *cam);
unsigned int cam_fps_mode(void);
int cam_fps_start(struct cam_device *cam);
void cam_experiment_begin(struct cam_device *cam);
void cam_experiment_end(struct cam_device *cam);
int cam_experiment_round(struct cam_device *cam);
int cam_experiment_load(struct cam_device *cam);
int cam_framing_start(struct cam_device *cam);
int cam_framing_requeue(struct cam_device *cam);
int cam_select(struct cam_device *cam);
int cam_current(struct cam_device *cam);
int cam_configure_buffers(struct cam_device *cam);
int cam_output_verify(struct cam_device *cam, bool final);
int cam_channel_input(struct cam_device *cam, u16 opcode, u32 value, bool argument);
int cam_metadata(struct cam_device *cam);
int cam_capture(struct cam_device *cam);
int cam_requeue(struct cam_device *cam, u32 meta, u32 images);
int cam_poweroff_sensor(struct cam_device *cam);
int cam_session_start(struct cam_device *cam);
void cam_session_stop(struct cam_device *cam);
void cam_stream_join(struct cam_device *cam);
void cam_idle_work(struct work_struct *work);
int cam_v4l2_register(struct cam_device *cam);
void cam_v4l2_unregister(struct cam_device *cam);
void cam_deliver(struct cam_device *cam, unsigned int slot, u64 timestamp);
void cam_return_buffers(struct cam_device *cam, enum vb2_buffer_state state);

#endif
