// SPDX-License-Identifier: GPL-2.0-only
/* The production STREAMON/STREAMOFF/shutdown callers, the lifecycle worker
 * and its steps, run against stubs with one kernel rule: in a task with a
 * pending signal an interruptible RTKit wait returns -ERESTARTSYS at once,
 * and with a fatal one kthread_run fails with -EINTR. The worker task never
 * has a signal. Single-threaded: a held mutex taken again would deadlock.
 */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int u32;
typedef unsigned long long u64;

#define __iomem
#define ERESTARTSYS 512
#define NSEC_PER_MSEC 1000000L
#define HZ 100
#define BIT(n) (1U << (n))
#define CAM_CONTROL 0x44
#define CAM_RUN BIT(4)
#define CAM_CAPTURE_SIZE 64
#define D_PMGR 0x100ULL
#define D_PMGR_SIZE 0x10UL
#define THIS_MODULE NULL
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, value) ((x) = (value))
#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define IS_ERR(pointer) ((uintptr_t)(pointer) >= (uintptr_t)-4095)
#define PTR_ERR(pointer) ((int)(intptr_t)(pointer))
#define container_of(pointer, type, member) \
	((type *)((char *)(pointer) - offsetof(type, member)))
#define lockdep_assert_held(lock) assert((lock)->owner == context)
#define lockdep_assert_not_held(lock) assert((lock)->owner != context)
#define spin_lock_irqsave(lock, flags) ((flags) = spin_take(lock))
#define spin_unlock_irqrestore(lock, flags) spin_give(lock, flags)
#define dev_err dev_info
#define dma_wmb() ((void)0)
#define WARN_ON(condition) warn_on(!!(condition))

enum task { NOBODY, CALLER, WORKER };
enum cam_state { CAM_PROBED, CAM_IDLE, CAM_STREAMING, CAM_DEAD };
enum vb2_buffer_state { VB2_BUF_STATE_DONE, VB2_BUF_STATE_ERROR, VB2_BUF_STATE_QUEUED };

struct mutex { enum task owner; };
struct test_spinlock { bool held; };
struct completion { unsigned int done; };
struct work_struct { void (*func)(struct work_struct *work); };
struct delayed_work { bool pending; };
struct workqueue_struct { int unused; };
struct vb2_queue { int unused; };
struct platform_device { int unused; };
struct task_struct { int unused; };
struct apple_mbox { unsigned char *regs; };
struct cam_geometry { u32 width, height, crop_y; };
struct cam_session { bool active; u64 start_ns; };

struct cam_device {
	void *dev;
	struct vb2_queue queue;
	struct mutex video_lock, fw_lock;
	struct test_spinlock service_lock;
	struct task_struct *thread;
	struct delayed_work idle_work;
	struct workqueue_struct *lifecycle;
	struct work_struct lifecycle_work;
	int (*lifecycle_step)(struct cam_device *cam);
	int lifecycle_result;
	enum cam_state state;
	char dead_reason[160];
	bool firmware_started, firmware_running, shutting_down, queue_ready, service_active;
	unsigned int sessions, fw_starts, fw_sessions;
	struct cam_geometry geometry;
	struct cam_session session;
	void *rtk;
	struct apple_mbox *mbox;
	unsigned char *cpu, *gpio;
	u32 control;
	u64 capture_base, last_stop_ns, poweroff_ns;
	struct completion frames, stream_started;
	u32 sensor_version, presets;
};

static enum task context = CALLER;
static bool signal_pending[3], fatal_pending[3];
static struct cam_device cam;
static struct workqueue_struct lifecycle_wq;
static struct task_struct stream_task;
static struct apple_mbox mbox;
static struct vb2_queue *const queue = &cam.queue;
static struct platform_device platform;
static unsigned char mbox_regs[0x200], gpio_regs[0x20], cpu_regs[0x100], capture[CAM_CAPTURE_SIZE];
static int rtk_instance, pmgr_instance, status_state = -1;
static char dead_reason[160], events[64];
static bool firmware_started, work_pending, running_after_wake;
static bool cam_fw_restart = true;
static unsigned int fw_starts, worker_runs, caller_steps, module_refs;
static int rearm_result, wake_result, bootstrap_result, setup_result, thread_result;
static int sleep_result, down_result;
static enum vb2_buffer_state returned;
static size_t event_count;
static u64 now_ns;

static void event(char code)
{
	assert(event_count + 1 < sizeof(events));
	events[event_count++] = code;
	events[event_count] = 0;
}

/* Every hardware and RTKit step: under fw_lock, counted if not on the worker. */
static void hw_step(char code)
{
	assert(cam.fw_lock.owner == context);
	if (code)
		event(code);
	caller_steps += context != WORKER;
}

static void dev_info(void *device, const char *format, ...)
{
	char line[256];
	va_list args;

	assert(!device);
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
}

static bool warn_on(bool condition)
{
	assert(!condition);
	return condition;
}

static void mutex_lock(struct mutex *lock)
{
	assert(lock->owner == NOBODY);
	lock->owner = context;
}

static void mutex_unlock(struct mutex *lock)
{
	assert(lock->owner == context);
	lock->owner = NOBODY;
}

static unsigned long spin_take(struct test_spinlock *lock)
{
	assert(!lock->held);
	lock->held = true;
	return 0x5a;
}

static void spin_give(struct test_spinlock *lock, unsigned long flags)
{
	assert(lock->held && flags == 0x5a);
	lock->held = false;
}

static void strscpy(char *destination, const char *source, size_t size)
{
	snprintf(destination, size, "%s", source);
}

static int vscnprintf(char *buffer, size_t size, const char *format, va_list args)
{
	int length = vsnprintf(buffer, size, format, args);

	return length < (int)size ? length : (int)size - 1;
}

static void complete_all(struct completion *done)
{
	done->done = ~0U;
}

static void reinit_completion(struct completion *done)
{
	done->done = 0;
}

static void vb2_queue_error(struct vb2_queue *error_queue)
{
	assert(error_queue == queue);
	event('E');
}

static struct cam_device *vb2_get_drv_priv(struct vb2_queue *driver_queue)
{
	assert(driver_queue == queue);
	return &cam;
}

static struct cam_device *platform_get_drvdata(struct platform_device *device)
{
	assert(device == &platform);
	return &cam;
}

static bool cam_dead(const struct cam_device *device)
{
	return device->state == CAM_DEAD;
}

static void cam_dead_dump(struct cam_device *device)
{
	assert(device->fw_lock.owner == context);
}

static void cam_print_drain(struct cam_device *device)
{
	assert(device->fw_lock.owner == context);
	event('D');
}

static void cam_publish_status(struct cam_device *device)
{
	assert(device->service_lock.held);
}

static u64 ktime_get_ns(void)
{
	return now_ns += NSEC_PER_MSEC;
}

static u32 readl(const void *address)
{
	u32 value;

	hw_step(0);
	memcpy(&value, address, sizeof(value));
	return value;
}

static void writel(u32 value, void *address)
{
	hw_step(address == cpu_regs + CAM_CONTROL ? (value & CAM_RUN ? 'R' : 'W') : 0);
	memcpy(address, &value, sizeof(value));
}

static void *ioremap(u64 address, unsigned long size)
{
	hw_step('M');
	assert(address == D_PMGR && size == D_PMGR_SIZE);
	return &pmgr_instance;
}

static void iounmap(void *pmgr)
{
	assert(pmgr == &pmgr_instance);
	event('m');
}

static void __module_get(void *module)
{
	assert(!module);
	module_refs++;
}

/* The re-arm's ordering is restart_test's; here only where it runs. */
static int cam_rearm(struct cam_device *device)
{
	hw_step('A');
	/* apple_rtkit_init's workqueue rescuer is a kthread: a killable wait. */
	if (fatal_pending[context] || rearm_result)
		return fatal_pending[context] ? -ENOMEM : rearm_result;
	device->rtk = &rtk_instance;
	return 0;
}

static int cam_check_image(struct cam_device *device, bool restore)
{
	(void)device;
	assert(restore);
	hw_step('C');
	return 0;
}

static int cam_cpu_irq_init(struct cam_device *device)
{
	(void)device;
	hw_step('Q');
	return 0;
}

static int apple_rtkit_wake(void *rtk)
{
	assert(rtk == &rtk_instance);
	hw_step('K');
	return signal_pending[context] ? -ERESTARTSYS : wake_result;
}

static bool apple_rtkit_is_running(void *rtk)
{
	assert(rtk == &rtk_instance);
	hw_step(0);
	return running_after_wake;
}

/* apple_rtkit_start_ep can wait interruptibly for mailbox space. */
static int cam_ipc_bootstrap(struct cam_device *device)
{
	(void)device;
	hw_step('B');
	return signal_pending[context] ? -ERESTARTSYS : bootstrap_result;
}

static int apple_rtkit_shutdown(void *rtk)
{
	assert(rtk == &rtk_instance);
	hw_step('S');
	return signal_pending[context] ? -ERESTARTSYS : sleep_result;
}

static void apple_rtkit_free(void *rtk)
{
	assert(rtk == &rtk_instance);
	hw_step('F');
}

static int d_power_down(void *pmgr)
{
	assert(pmgr == &pmgr_instance);
	hw_step('O');
	return down_result;
}

static bool d_power_at_boot_state(void *pmgr)
{
	assert(pmgr == &pmgr_instance);
	hw_step('Z');
	return true;
}

static void *cam_pointer(struct cam_device *device, u64 address, size_t size)
{
	assert(address == device->capture_base && size == CAM_CAPTURE_SIZE);
	return capture;
}

static int cam_session_setup(struct cam_device *device)
{
	(void)device;
	hw_step('U');
	return setup_result;
}

static void cam_experiment_begin(struct cam_device *device)
{
	assert(device->fw_lock.owner == context);
}

static int cam_stream_thread(void *data)
{
	(void)data;
	abort();
}

static struct task_struct *kthread_run(int (*function)(void *data), void *data, const char *name)
{
	assert(function == cam_stream_thread && data == &cam && !strcmp(name, "j713-camera"));
	hw_step('N');
	if (fatal_pending[context])
		return ERR_PTR(-EINTR);
	return thread_result ? ERR_PTR(thread_result) : &stream_task;
}

static void cam_session_stop(struct cam_device *device)
{
	assert(device->fw_lock.owner == context);
	event('X');
}

static void cam_stream_join(struct cam_device *device)
{
	assert(context == CALLER && device->video_lock.owner == CALLER);
	assert(device->fw_lock.owner == NOBODY);
	event('J');
	device->thread = NULL;
}

static void cam_set_state(struct cam_device *device, enum cam_state state)
{
	assert(context == CALLER && device->video_lock.owner == CALLER);
	if (!cam_dead(device))
		device->state = state;
}

static void cam_return_buffers(struct cam_device *device, enum vb2_buffer_state state)
{
	assert(context == CALLER && device->video_lock.owner == CALLER);
	event('r');
	returned = state;
}

static void cancel_delayed_work_sync(struct delayed_work *work)
{
	assert(context == CALLER && work == &cam.idle_work);
	work->pending = false;
}

static bool schedule_delayed_work(struct delayed_work *work, unsigned long delay)
{
	assert(work == &cam.idle_work && delay == HZ);
	work->pending = true;
	return true;
}

static void wait_for_completion(struct completion *done)
{
	assert(context == CALLER && done == &cam.stream_started);
	assert(cam.fw_lock.owner == NOBODY && cam.thread == &stream_task);
	event('T');
}

static bool queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	assert(wq == &lifecycle_wq && work == &cam.lifecycle_work);
	if (work_pending)
		return false;
	work_pending = true;
	return true;
}

/* The caller's uninterruptible wait: it runs the work in the worker task. */
static bool flush_work(struct work_struct *work)
{
	assert(context == CALLER && work == &cam.lifecycle_work);
	assert(cam.video_lock.owner == CALLER && cam.fw_lock.owner == NOBODY);
	/* No stream thread holds fw_lock, and the idle work is joined. */
	assert(!cam.thread && (!cam.idle_work.pending || cam.shutting_down));
	if (!work_pending)
		return false;
	work_pending = false;
	context = WORKER;
	worker_runs++;
	work->func(work);
	assert(cam.fw_lock.owner == NOBODY);
	context = CALLER;
	return true;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include ".build/lifecycle_impl.h"
#pragma GCC diagnostic pop

/* Firmware running and streaming, or off after a completed power-stop. */
static void reset(bool running)
{
	u32 empty = BIT(17);

	cam = (struct cam_device) {
		.lifecycle = &lifecycle_wq, .lifecycle_work = { .func = cam_lifecycle_work },
		.state = running ? CAM_STREAMING : CAM_IDLE, .firmware_started = true,
		.firmware_running = running, .queue_ready = true, .service_active = running,
		.rtk = running ? &rtk_instance : NULL, .mbox = &mbox, .cpu = cpu_regs,
		.gpio = gpio_regs, .fw_starts = 1, .fw_sessions = 1, .sessions = 1,
		.thread = running ? &stream_task : NULL, .idle_work = { .pending = running },
		.geometry = { 1280, 720, 0 }, .capture_base = 0x1000, .last_stop_ns = 1,
		.poweroff_ns = running ? 0 : 1,
	};
	mbox.regs = mbox_regs;
	memcpy(mbox_regs + 0x110, &empty, sizeof(empty));
	memcpy(mbox_regs + 0x114, &empty, sizeof(empty));
	memset(signal_pending, 0, sizeof(signal_pending));
	memset(fatal_pending, 0, sizeof(fatal_pending));
	context = CALLER;
	running_after_wake = true;
	cam_fw_restart = true;
	work_pending = false;
	worker_runs = 0;
	caller_steps = 0;
	rearm_result = 0;
	wake_result = 0;
	bootstrap_result = 0;
	setup_result = 0;
	thread_result = 0;
	sleep_result = 0;
	down_result = 0;
	returned = VB2_BUF_STATE_DONE;
	status_state = -1;
	strscpy(dead_reason, "none", sizeof(dead_reason));
	event_count = 0;
	events[0] = 0;
}

static void signal_caller(void)
{
	signal_pending[CALLER] = true;
	fatal_pending[CALLER] = true;
}

static int stream_on(void)
{
	int ret;

	mutex_lock(&cam.video_lock);
	ret = cam_start_streaming(queue, 0);
	mutex_unlock(&cam.video_lock);
	return ret;
}

static void stream_off(void)
{
	mutex_lock(&cam.video_lock);
	cam_stop_streaming(queue);
	mutex_unlock(&cam.video_lock);
}

/* Without the worker, the boot 3 failure and its STREAMON twins. */
static void caller_task_fails(void)
{
	reset(true);
	signal_caller();
	mutex_lock(&cam.fw_lock);
	assert(cam_fw_stop(&cam) == -ERESTARTSYS);
	mutex_unlock(&cam.fw_lock);
	assert(!strcmp(events, "DSDE") && cam.firmware_running && cam.rtk);
	assert(!strcmp(dead_reason, "RTKit sleep failed (-512) before the power-stop"));
	reset(false);
	signal_pending[CALLER] = true;
	cam.state = CAM_STREAMING;
	mutex_lock(&cam.fw_lock);
	assert(cam_boot(&cam) == -ERESTARTSYS);
	mutex_unlock(&cam.fw_lock);
	assert(!strcmp(events, "ACQRKE"));
	assert(!strcmp(dead_reason, "RTKit wake failed (-512) at firmware boot"));
	reset(true);
	cam.thread = NULL;
	signal_caller();
	mutex_lock(&cam.fw_lock);
	assert(cam_session_start(&cam) == -EIO);
	mutex_unlock(&cam.fw_lock);
	assert(!strcmp(events, "DUNEX") && !cam.thread);
	assert(!strcmp(dead_reason, "session start failed"));
}

static void streamoff_with_signal(void)
{
	reset(true);
	signal_caller();
	stream_off();
	assert(!strcmp(events, "JDSDWFMOZmr") && worker_runs == 1 && !caller_steps);
	assert(!cam_dead(&cam) && cam.state == CAM_IDLE && !cam.firmware_running);
	assert(!cam.rtk && !cam.service_active && cam.poweroff_ns == now_ns);
	assert(returned == VB2_BUF_STATE_ERROR && !cam.idle_work.pending);
	assert(!strcmp(dead_reason, "none"));
}

static void streamon_with_signal(void)
{
	reset(false);
	signal_caller();
	assert(!stream_on());
	assert(!strcmp(events, "ACQRKBDUNT") && worker_runs == 2 && !caller_steps);
	assert(cam.state == CAM_STREAMING && cam.firmware_running && cam.fw_starts == 2);
	assert(fw_starts == 2 && cam.thread == &stream_task && cam.idle_work.pending);
	assert(cam.sessions == 2 && cam.fw_sessions == 1 && cam.rtk == &rtk_instance);
	assert(!strcmp(dead_reason, "none"));
	/* With the firmware already running only the session step is queued. */
	reset(true);
	cam.thread = NULL;
	cam.idle_work.pending = false;
	cam.state = CAM_IDLE;
	signal_caller();
	assert(!stream_on() && !strcmp(events, "DUNT") && worker_runs == 1 && !caller_steps);
}

static void shutdown_with_signal(void)
{
	reset(true);
	signal_caller();
	cam_shutdown(&platform);
	assert(!strcmp(events, "JXS") && worker_runs == 1 && !caller_steps);
	assert(!cam_dead(&cam) && !cam.firmware_running && cam.shutting_down);
	assert(cam.video_lock.owner == NOBODY && !cam.idle_work.pending);
	/* Between sessions the firmware is off and only the stop is repeated. */
	reset(false);
	cam_shutdown(&platform);
	assert(!strcmp(events, "JX") && worker_runs == 1 && !cam_dead(&cam));
}

static void worker_failures(void)
{
	reset(true);
	sleep_result = -ETIME;
	stream_off();
	assert(!strcmp(events, "JDSDEr") && cam_dead(&cam) && cam.firmware_running);
	assert(cam.rtk && !cam.poweroff_ns && returned == VB2_BUF_STATE_ERROR);
	assert(!strcmp(dead_reason, "RTKit sleep failed (-62) before the power-stop"));
	assert(!cam.idle_work.pending && status_state == CAM_DEAD);
	reset(false);
	wake_result = -ETIME;
	assert(stream_on() == -ETIME && returned == VB2_BUF_STATE_QUEUED);
	assert(!strcmp(events, "ACQRKEr") && worker_runs == 1);
	assert(!strcmp(dead_reason, "RTKit wake failed (-62) at firmware boot"));
	reset(false);
	bootstrap_result = -ETIMEDOUT;
	assert(stream_on() == -ETIMEDOUT && !strcmp(events, "ACQRKBEr"));
	assert(!strcmp(dead_reason, "firmware boot failed (-110)"));
	reset(false);
	running_after_wake = false;
	assert(stream_on() == -EIO && !strcmp(dead_reason, "firmware boot failed (-5)"));
	reset(false);
	thread_result = -ENOMEM;
	assert(stream_on() == -EIO && !strcmp(events, "ACQRKBDUNEXr"));
	assert(!strcmp(dead_reason, "session start failed") && !cam.thread);
	reset(true);
	sleep_result = -ETIME;
	cam_shutdown(&platform);
	assert(!strcmp(events, "JXSE") && cam_dead(&cam) && cam.firmware_running);
	assert(!strcmp(dead_reason, "RTKit sleep failed (-62) at shutdown"));
	reset(true);
	down_result = -EIO;
	stream_off();
	assert(!strcmp(events, "JDSDWFMOmEr") && !cam.poweroff_ns);
	assert(!strcmp(dead_reason, "ISP sub-domains did not all power down"));
}

static void no_worker_needed(void)
{
	reset(true);
	cam_fw_restart = false;
	stream_off();
	assert(!strcmp(events, "Jr") && !worker_runs && cam.firmware_running);
	assert(cam.state == CAM_IDLE && cam.idle_work.pending);
	reset(false);
	cam.state = CAM_DEAD;
	assert(stream_on() == -EIO && !strcmp(events, "r") && !worker_runs);
}

int main(void)
{
	caller_task_fails();
	streamoff_with_signal();
	streamon_with_signal();
	shutdown_with_signal();
	worker_failures();
	no_worker_needed();
	puts("lifecycle tests passed: signalled STREAMOFF/STREAMON/shutdown complete on the worker, coded RTKit reasons, lock ownership");
	return 0;
}
