// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;

#define __iomem
#define NSEC_PER_MSEC 1000000ULL
#define DIV_ROUND_UP_ULL(n, d) (((n) + (d) - 1) / (d))
#define CAM_CONTROL 0x44
#define D_PMGR 1
#define D_PMGR_SIZE 1
#define HZ 100
#define ERESTARTSYS 512
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define WRITE_ONCE(x, value) ((x) = (value))
#define READ_ONCE(x) ({ track_read(&(x)); (x); })
#define IS_ERR(pointer) ((intptr_t)(pointer) < 0)
#define PTR_ERR(pointer) ((int)(intptr_t)(pointer))
#define dev_info(...) ((void)0)
#define lockdep_assert_held(lock) assert(*(lock))
#define to_delayed_work(work) (work)
#define container_of(pointer, type, member) \
	((type *)((char *)(pointer) - offsetof(type, member)))

enum cam_state { CAM_IDLE, CAM_STREAMING, CAM_DEAD };
struct work_struct { int unused; };
struct kernel_param { void *arg; };
struct cam_device {
	bool fw_lock, firmware_running, shutting_down, service_active;
	void *rtk, *thread, *dev;
	unsigned char *cpu;
	unsigned int control, fw_starts;
	u64 poweroff_ns;
	enum cam_state state;
	struct work_struct idle_work;
};

static unsigned int restart_off_ms, restart_settle_ms, off_reads, settle_reads;
static struct cam_device cam;
static unsigned char cpu[0x100];
static bool powered, free_dead, bad_guard, retune, short_sleep, busy_lock;
static bool state_changes_at_lock, idle_scheduled;
static int sleep_result, down_result, power_result, init_result;
static u64 now_ns, required_off_ns;
static unsigned int on_sleep_ms;
static char events[128], last_reason[160];
static size_t event_count;

static void event(char code)
{
	assert(event_count + 1 < sizeof(events));
	events[event_count++] = code;
	events[event_count] = 0;
}

static void track_read(const void *value)
{
	if (value == &restart_off_ms)
		off_reads++;
	if (value == &restart_settle_ms)
		settle_reads++;
}

static int kstrtouint(const char *value, unsigned int base, unsigned int *result)
{
	unsigned long long parsed;
	char *end;

	if (*value == '-')
		return -EINVAL;
	errno = 0;
	parsed = strtoull(value, &end, base);
	if (errno || parsed > UINT32_MAX)
		return -ERANGE;
	if (end == value || (*end && strcmp(end, "\n")))
		return -EINVAL;
	*result = parsed;
	return 0;
}

static bool cam_dead(struct cam_device *device)
{
	return device->state == CAM_DEAD;
}

static void cam_fail(struct cam_device *device, const char *reason)
{
	assert(reason);
	if (!cam_dead(device))
		snprintf(last_reason, sizeof(last_reason), "%s", reason);
	device->state = CAM_DEAD;
	event('!');
}

static void cam_failf(struct cam_device *device, const char *format, ...)
{
	char reason[sizeof(last_reason)];
	va_list args;

	va_start(args, format);
	vsnprintf(reason, sizeof(reason), format, args);
	va_end(args);
	cam_fail(device, reason);
}

static void cam_dead_dump(struct cam_device *device)
{
	(void)device;
}

static void cam_guard_audit(struct cam_device *device)
{
	assert(!powered);
	event('G');
	if (bad_guard)
		cam_fail(device, "guard");
}

static void cam_reset_memory(struct cam_device *device)
{
	assert(!powered && !device->rtk && !device->thread && !cam_dead(device));
	event('Z');
	now_ns += 80 * NSEC_PER_MSEC;
}

static u64 ktime_get_ns(void)
{
	return now_ns;
}

static void msleep(unsigned int milliseconds)
{
	if (powered)
		on_sleep_ms += milliseconds;
	if (retune) {
		restart_off_ms = 0;
		restart_settle_ms = 0;
	}
	if (short_sleep && !powered) {
		milliseconds /= 2;
		short_sleep = false;
	}
	now_ns += (u64)milliseconds * NSEC_PER_MSEC;
}

static int cam_take_power(struct cam_device *device, bool *cycled)
{
	assert(!powered && now_ns >= device->poweroff_ns + required_off_ns);
	event('U');
	powered = true;
	*cycled = true;
	return power_result;
}

static int cam_check_power(void *dev)
{
	(void)dev;
	assert(powered);
	event('P');
	return 0;
}

static int cam_check_cpu(struct cam_device *device, bool cycled)
{
	(void)device;
	assert(powered && cycled);
	event('C');
	return 0;
}

static const int cam_rtkit_ops;

static void *apple_rtkit_init(void *dev, void *cookie, const char *name,
			      int index, const void *ops)
{
	(void)dev;
	(void)cookie;
	assert(powered && !strcmp(name, "rtkit") && !index && ops);
	event('I');
	return init_result ? (void *)(intptr_t)init_result : &cam;
}

static void cam_print_drain(struct cam_device *device)
{
	assert(powered && device->rtk);
	event('D');
}

static int apple_rtkit_shutdown(void *rtk)
{
	assert(powered && rtk);
	event('S');
	return sleep_result;
}

static void writel(unsigned int value, void *address)
{
	assert(powered && !cam.firmware_running && value == cam.control);
	assert(address == cpu + CAM_CONTROL);
	event('W');
}

static void apple_rtkit_free(void *rtk)
{
	assert(powered && rtk && !cam.firmware_running);
	event('F');
	if (free_dead)
		cam_fail(&cam, "last worker callback");
}

static void *ioremap(unsigned int address, unsigned int size)
{
	assert(powered && !cam.rtk && address == D_PMGR && size == D_PMGR_SIZE);
	event('M');
	return &cam;
}

static void iounmap(void *pmgr)
{
	assert(pmgr);
	event('m');
}

static int d_power_down(void *pmgr)
{
	(void)pmgr;
	assert(powered && !cam.rtk && !cam_dead(&cam));
	event('O');
	powered = false;
	now_ns += 200 * NSEC_PER_MSEC;
	return down_result;
}

static bool d_power_at_boot_state(void *pmgr)
{
	(void)pmgr;
	assert(!powered);
	event('B');
	return true;
}

static bool mutex_trylock(bool *lock)
{
	if (state_changes_at_lock) {
		cam.firmware_running = false;
		cam.state = CAM_IDLE;
		powered = false;
	}
	if (busy_lock)
		return false;
	*lock = true;
	return true;
}

static void mutex_unlock(bool *lock)
{
	*lock = false;
}

static void schedule_delayed_work(struct work_struct *work, unsigned int delay)
{
	(void)work;
	assert(delay == HZ);
	idle_scheduled = true;
}

#include ".build/restart_impl.h"

static void reset(void)
{
	cam = (struct cam_device) {
		.fw_lock = true, .firmware_running = true, .rtk = &cam,
		.service_active = true, .cpu = cpu, .state = CAM_IDLE,
	};
	now_ns = NSEC_PER_MSEC;
	free_dead = false;
	bad_guard = false;
	retune = false;
	short_sleep = false;
	busy_lock = false;
	state_changes_at_lock = false;
	idle_scheduled = false;
	sleep_result = 0;
	down_result = 0;
	power_result = 0;
	init_result = 0;
	powered = true;
	on_sleep_ms = 0;
	off_reads = 0;
	settle_reads = 0;
	restart_off_ms = 5000;
	restart_settle_ms = 3000;
	required_off_ns = 5000 * NSEC_PER_MSEC;
	event_count = 0;
	events[0] = 0;
	last_reason[0] = 0;
}

static void parameters(void)
{
	struct kernel_param off = { .arg = &restart_off_ms };
	struct kernel_param settle = { .arg = &restart_settle_ms };
	const char * const invalid[] = { "10001", "-1", "4294967296", "garbage" };
	size_t i;

	assert(!cam_restart_ms_set("10000", &off) && restart_off_ms == 10000);
	assert(!cam_restart_ms_set("5000", &settle) && restart_settle_ms == 5000);
	assert(cam_restart_ms_set("5001", &settle) == -ERANGE && restart_settle_ms == 5000);
	for (i = 0; i < ARRAY_SIZE(invalid); i++) {
		assert(cam_restart_ms_set(invalid[i], &off));
		assert(restart_off_ms == 10000);
	}
	assert(!cam_restart_ms_set("0\n", &off) && !restart_off_ms);
	assert(!cam_restart_ms_set("0", &settle) && !restart_settle_ms);
}

static void healthy_restart(void)
{
	reset();
	assert(!cam_fw_stop(&cam));
	assert(!strcmp(events, "DSDWFMOBm"));
	assert(!powered && !cam.rtk && !cam.firmware_running && !cam.service_active);
	assert(cam.poweroff_ns == now_ns && !cam_dead(&cam));
	retune = true;
	short_sleep = true;
	assert(!cam_rearm(&cam));
	assert(!strcmp(events, "DSDWFMOBmGZUPCI"));
	assert(off_reads == 1 && settle_reads == 1 && on_sleep_ms == 3000);
	assert(now_ns == cam.poweroff_ns + 8000 * NSEC_PER_MSEC);
	assert(!restart_off_ms && !restart_settle_ms && cam.rtk);
	reset();
	cam_fw_stop(&cam);
	now_ns += 6000 * NSEC_PER_MSEC;
	assert(!cam_rearm(&cam));
	assert(now_ns == cam.poweroff_ns + 9080 * NSEC_PER_MSEC);
	reset();
	cam_fw_stop(&cam);
	restart_off_ms = 0;
	restart_settle_ms = 0;
	required_off_ns = 0;
	assert(!cam_rearm(&cam) && !on_sleep_ms);
	assert(now_ns == cam.poweroff_ns + 80 * NSEC_PER_MSEC);
}

static void failed_restart(void)
{
	reset();
	sleep_result = -ERESTARTSYS;
	assert(cam_fw_stop(&cam) == -ERESTARTSYS);
	assert(!strcmp(events, "DSD!") && powered && cam.rtk && cam_dead(&cam));
	assert(!strcmp(last_reason, "RTKit sleep failed (-512) before the power-stop"));
	reset();
	free_dead = true;
	assert(cam_fw_stop(&cam) == -EIO);
	assert(!strcmp(events, "DSDWF!") && powered && !cam.rtk && cam_dead(&cam));
	reset();
	down_result = -EIO;
	assert(cam_fw_stop(&cam) == -EIO);
	assert(!cam.poweroff_ns && cam_dead(&cam));
	assert(cam_rearm(&cam) == -EIO);
	reset();
	cam_fw_stop(&cam);
	bad_guard = true;
	assert(cam_rearm(&cam) == -EIO && !powered && cam_dead(&cam));
	assert(!strcmp(events, "DSDWFMOBmG!"));
	reset();
	cam_fw_stop(&cam);
	power_result = -EIO;
	assert(cam_rearm(&cam) == -EIO && cam_dead(&cam) && !cam.rtk);
	assert(!strcmp(events, "DSDWFMOBmGZU!"));
	reset();
	cam_fw_stop(&cam);
	init_result = -ENOMEM;
	assert(cam_rearm(&cam) == -ENOMEM && cam_dead(&cam) && !cam.rtk);
	assert(!strcmp(last_reason, "RTKit init failed (-12) at re-arm"));
}

static void idle_transition(void)
{
	reset();
	state_changes_at_lock = true;
	cam_idle_work(&cam.idle_work);
	assert(!event_count && !idle_scheduled);
	reset();
	busy_lock = true;
	cam_idle_work(&cam.idle_work);
	assert(!event_count && idle_scheduled);
	reset();
	cam_idle_work(&cam.idle_work);
	assert(!strcmp(events, "D") && idle_scheduled);
}

int main(void)
{
	parameters();
	healthy_restart();
	failed_restart();
	idle_transition();
	puts("restart tests passed: production setters, power-stop/re-arm order, timing snapshots, DEAD during free, coded RTKit reasons, idle transition");
	return 0;
}
