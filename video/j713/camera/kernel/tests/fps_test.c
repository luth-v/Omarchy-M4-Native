// SPDX-License-Identifier: GPL-2.0-only
/* Actual firmware/session and V4L2 callbacks run against synthetic data. */
#include <linux/videodev2.h>

#include "experiment_harness.h"

struct file { int unused; };
static inline struct cam_device *video_drvdata(struct file *file)
{
	(void)file;
	return &cam;
}
#define vb2_is_streaming(queue) ((queue)->streaming)
#include ".build/fps_params_impl.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include ".build/fps_v4l2_impl.h"
#pragma GCC diagnostic pop
#if !CAM_HAS_FPS
#define cam_set_parm cam_parm
#else
#include ".build/experiment_defaults.h"
#endif

static struct v4l2_streamparm parameter(unsigned int numerator, unsigned int denominator)
{
	struct v4l2_streamparm parm;

	memset(&parm, 0xa5, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.timeperframe = (struct v4l2_fract) { numerator, denominator };
	return parm;
}

static struct v4l2_frmivalenum interval(unsigned int index)
{
	struct v4l2_frmivalenum value;

	memset(&value, 0xa5, sizeof(value));
	value.index = index;
	value.pixel_format = V4L2_PIX_FMT_NV12;
	value.width = CAM_FRAME_WIDTH;
	value.height = CAM_DELIVER_HEIGHT;
	return value;
}

#if CAM_HAS_FPS
static void validation(void)
{
	const char * const valid[] = { "0", "1", "30", "60", "0\n", "1\n", "30\n", "60\n" };
	const unsigned int values[] = { 0, 1, 30, 60, 0, 1, 30, 60 };
	const char * const invalid[] = { "", "2", "20", "29", "31", "59", "61", "120", "-1", "+30",
		"030", "0x1e", " 30", "30 ", "30\n\n", "30\r\n", "1x", "60x", "30;id" };
	struct kernel_param param = { &cam_experiment_config.fps };
	unsigned int i;

	reset(720);
	assert(!cam_fps_mode());
	for (i = 0; i < ARRAY_SIZE(valid); i++) {
		assert(!cam_fps_set(valid[i], &param));
		assert(cam_fps_mode() == values[i] && !cam_experiment_lock);
	}
	for (i = 0; i < ARRAY_SIZE(invalid); i++) {
		assert(cam_fps_set(invalid[i], &param) == -EINVAL);
		assert(cam_fps_mode() == 60 && !cam_experiment_lock);
	}
}

static void request(unsigned int index, u16 opcode, unsigned int rate)
{
	struct trace *t = &trace[index];
	u8 expected[CAM_QUERY_CAPACITY] = { };
	unsigned int size = opcode == 0x260 ? 0x20 : opcode == 0x207 || opcode == 0x209 ? 0xc : 0x10;

	put_unaligned_le16(opcode, expected + 4);
	/* All channel and reserved bytes must be zero, including the selector. */
	if (opcode == 0x260) {
		put_unaligned_le32(rate, expected + 0x10);
		put_unaligned_le32(1, expected + 0x14);
		put_unaligned_le32(rate, expected + 0x18);
		put_unaligned_le32(1, expected + 0x1c);
	} else if (opcode == 0x208 || opcode == 0x20a) {
		put_unaligned_le32(rate * 256, expected + 0xc);
	}
	assert(t->endpoint == 0x21 && t->size == size);
	assert(t->argument == (size == 0xc ? 0x10 : size));
	assert(!memcmp(t->command, expected, sizeof(expected)));
}

static void wire_case(unsigned int height, unsigned int session, unsigned int colour,
		      unsigned int mode, unsigned int selected)
{
	unsigned int start, max, min, i;
	unsigned int rate = mode == 1 ? selected : mode;
	char line[160];

	reset(height);
	cam.fw_sessions = session;
	cam.frame_rate = selected;
	cam_experiment_config.fps = mode;
	cam_experiment_config.colour = colour;
	assert(!cam_session_start(&cam) && !cam_dead(&cam));
	assert(cam.session.experiment.frame_rate == rate);
	if (!rate) {
		assert(!count_opcode(0x260) && !count_opcode(0x207) && !count_opcode(0x209));
		assert(!strstr(logs, "fps:"));
	} else {
		start = index_opcode(0x260, 0);
		max = index_opcode(0x207, 0);
		min = index_opcode(0x209, 0);
		assert(start > index_opcode(0x100, 0));
		if (colour)
			assert(start > index_opcode(0x111, 0));
		assert(max == start + 1 && min == max + 1);
		assert(min == trace_count - 2 && trace[min + 1].endpoint == 0x24);
		request(start, 0x260, rate);
		request(max, 0x207, 0);
		request(min, 0x209, 0);
		snprintf(line, sizeof(line), "fps: requested=%u pin_result=0 min_q8=%u min_result=0 max_q8=%u max_result=0\n",
			 rate, rate * 256, rate * 256);
		assert(strstr(logs, line));
	}
	i = count_opcode(0x260);
	run_stream();
	assert(count_opcode(0x260) == i && !count_opcode(0x208) && !count_opcode(0x20a));
	assert(!cam_dead(&cam));
}

static void wire_and_order(void)
{
	const unsigned int modes[] = { 0, 1, 30, 60 };
	unsigned int height, mode, selected, session, colour;

	for (height = 720; height <= 1264; height += 544)
		for (session = 0; session < 2; session++)
			for (colour = 0; colour <= 1; colour++)
				for (mode = 0; mode < ARRAY_SIZE(modes); mode++)
					for (selected = 30; selected <= 60; selected += 30)
						wire_case(height, session, colour, modes[mode], selected);
	reset(720);
	cam_experiment_config.fps = 60;
	fps_clamp = true;
	assert(!cam_session_start(&cam));
	assert(strstr(logs, "requested=60 pin_result=0 min_q8=7680 min_result=0 max_q8=7680 max_result=0"));
	cam_session_stop(&cam);
}

static unsigned int target(bool fallback)
{
	unsigned int result;

	reset(720);
	cam_experiment_config.fps = 60;
	assert(!cam_session_start(&cam));
	result = index_opcode(0x260, 0) + 1;
	cam_session_stop(&cam);
	if (fallback) {
		reset(720);
		cam_experiment_config.fps = 60;
		reject_at = result;
		assert(!cam_session_start(&cam));
		/* Range GETs, then the pair; 60 >= the 60 maximum raises max first. */
		request(result, 0x207, 0);
		request(result + 1, 0x209, 0);
		request(result + 2, 0x208, 60);
		request(result + 3, 0x20a, 60);
		assert(index_opcode(0x207, 1) == result + 4 && index_opcode(0x209, 1) == result + 5);
		assert(strstr(logs, "0x260 rejected, trying 0x208/0x20a pair"));
		assert(strstr(logs, "fps: pair order max-first: max_q8=15360 max_result=0 min_q8=3840 min_result=0\n"));
		assert(strstr(logs, "requested=60 pin_result=0 min_q8=15360 min_result=0 max_q8=15360 max_result=0"));
		cam_session_stop(&cam);
	}
	return result;
}

static void reply_policy(void)
{
	unsigned int first = target(true), step, fallback, kind, sends;

	for (fallback = 0; fallback < 2; fallback++)
		for (step = 0; step < (fallback ? 6U : 3U); step++)
			for (kind = 0; kind < 3; kind++) {
				unsigned int at = first + step + fallback;

				reset(720);
				cam_experiment_config.fps = 60;
				if (fallback)
					reject_at = first;
				if (!kind) {
					if (fallback)
						reject_also_at = at;
					else
						reject_at = at;
				} else if (kind == 1) {
					timeout_at = at;
				} else {
					dead_after_send = at;
				}
				if (kind) {
					assert(cam_session_start(&cam) == -EIO && cam_dead(&cam));
					/* A fatal range GET stops before choosing an order. */
					if (fallback && step < 2)
						assert(!strstr(logs, "pair order"));
					if (kind == 1)
						assert(cam.query_stalled && trace_count == at);
					/* Responsive DEAD cleanup may STOP, but never sends another FPS command. */
					for (sends = at; sends < trace_count; sends++) {
						u16 op = get_unaligned_le16(trace[sends].command + 4);

						assert(op != 0x260 && op != 0x208 && op != 0x20a && op != 0x207 && op != 0x209);
					}
				} else {
					assert(!cam_session_start(&cam) && !cam_dead(&cam) && !failures);
					/* A rejected range GET falls back to min first. */
					if (fallback && step < 2) {
						assert(strstr(logs, "pair order min-first"));
						assert(index_opcode(0x20a, 0) + 1 == index_opcode(0x208, 0));
						assert(strstr(logs, "requested=60 pin_result=0 min_q8=15360 min_result=0 max_q8=15360 max_result=0"));
					}
					if (fallback && (step == 2 || step == 3)) {
						assert(strstr(logs, "pin rejected, giving up for session"));
						assert(count_opcode(0x20a) == (step == 3));
						assert(strstr(logs, "requested=60 pin_result=-71 min_q8=3840 min_result=0 max_q8=15360 max_result=0"));
					}
					sends = count_opcode(0x260) + count_opcode(0x208) + count_opcode(0x20a);
					run_stream();
					assert(!cam_dead(&cam) && !failures);
					assert(sends == count_opcode(0x260) + count_opcode(0x208) + count_opcode(0x20a));
				}
			}
	reset(720);
	cam_experiment_config.fps = 30;
	reject_at = first + 1;
	reject_also_at = first + 2;
	assert(!cam_session_start(&cam));
	assert(strstr(logs, "min_q8=0 min_result=-71 max_q8=0 max_result=-71"));
	cam_session_stop(&cam);
	reset(720);
	cam_experiment_config.fps = 30;
	timeout_at = first - 1;
	assert(cam_session_start(&cam) == -EIO && !count_opcode(0x260));
	reset(720);
	cam.session.experiment.frame_rate = 30;
	cam.state = CAM_DEAD;
	assert(!cam_fps_start(&cam) && !trace_count);
}

/* A rejected 0x260 reads the range, then raises max first or lowers min first. */
static void pair_order(void)
{
	static const struct {
		unsigned int rate, min, max, zero, reject;
		bool max_first;
	} ranges[] = {
		{ 60, 15, 60, 0, 0, true },	/* inferred default range, rate at the maximum */
		{ 60, 30, 30, 0, 0, true },	/* raise a range pinned at 30 */
		{ 30, 30, 30, 0, 0, true },	/* equal to the maximum */
		{ 30, 15, 60, 0, 0, false },	/* lower the default maximum */
		{ 30, 60, 60, 0, 0, false },	/* lower a range pinned at 60 */
		{ 30, 45, 60, 0, 0, false },	/* minimum above the rate */
		{ 60, 15, 60, 1, 0, false },	/* MAX_GET reads 0 */
		{ 60, 15, 60, 2, 0, false },	/* MIN_GET reads 0 */
		{ 60, 15, 60, 0, 1, false },	/* MAX_GET rejected */
		{ 60, 15, 60, 0, 2, false },	/* MIN_GET rejected */
	};
	unsigned int first = target(false), i;
	char line[160];

	for (i = 0; i < ARRAY_SIZE(ranges); i++) {
		unsigned int rate = ranges[i].rate, zero = ranges[i].zero, reject = ranges[i].reject;
		u16 order = ranges[i].max_first ? 0x208 : 0x20a;

		reset(720);
		cam_experiment_config.fps = rate;
		fps_min_reply = ranges[i].min * 256;
		fps_max_reply = ranges[i].max * 256;
		fps_zero_reads = zero;
		reject_at = first;
		if (reject)
			reject_also_at = first + reject;
		/* The harness asserts min <= max after every applied SET. */
		assert(!cam_session_start(&cam) && !cam_dead(&cam) && !failures);
		request(first, 0x207, 0);
		request(first + 1, 0x209, 0);
		request(first + 2, order, rate);
		request(first + 3, order ^ 0x208 ^ 0x20a, rate);
		request(first + 4, 0x207, 0);
		request(first + 5, 0x209, 0);
		assert(trace[first + 6].endpoint == 0x24 && first + 7 == trace_count);
		snprintf(line, sizeof(line), "fps: pair order %s: max_q8=%u max_result=%d min_q8=%u min_result=%d\n",
			 ranges[i].max_first ? "max-first" : "min-first",
			 zero & 1 || reject == 1 ? 0 : ranges[i].max * 256, reject == 1 ? -EPROTO : 0,
			 zero & 2 || reject == 2 ? 0 : ranges[i].min * 256, reject == 2 ? -EPROTO : 0);
		assert(strstr(logs, line));
		snprintf(line, sizeof(line), "fps: requested=%u pin_result=0 min_q8=%u min_result=0 max_q8=%u max_result=0\n",
			 rate, zero & 2 ? 0 : rate * 256, zero & 1 ? 0 : rate * 256);
		assert(strstr(logs, line) && !strstr(logs, "pin rejected"));
		cam_session_stop(&cam);
	}
}

static void latch(void)
{
	unsigned int moment, mode;
	const unsigned int modes[] = { 0, 1, 30, 60 };

	for (mode = 0; mode < ARRAY_SIZE(modes); mode++)
		for (moment = 0; moment < 2; moment++) {
			reset(720);
			cam_experiment_config.fps = modes[mode];
			cam_experiment_config.colour = 1;
			cam.frame_rate = 60;
			retune_at_request = !moment;
			retune_at_send = moment;
			assert(!cam_session_start(&cam));
			assert(cam.session.experiment.config.fps == modes[mode]);
			assert(cam.session.experiment.frame_rate == (modes[mode] == 1 ? 60 : modes[mode]));
			assert(!cam_experiment_config.fps);
			run_stream();
		}
	reset(720);
	cam_experiment_config.fps = 1;
	assert(!cam_session_start(&cam) && cam.session.experiment.frame_rate == 30);
	run_stream();
	cam.frame_rate = 60;
	cam_experiment_config.fps = 1;
	thread_stop = false;
	assert(!cam_session_start(&cam) && cam.session.experiment.frame_rate == 60);
	run_stream();
	cam_experiment_config.fps = 30;
	thread_stop = false;
	assert(!cam_session_start(&cam) && cam.session.experiment.frame_rate == 30);
	run_stream();
}

static void check_parm(struct v4l2_streamparm *parm, unsigned int rate)
{
	struct v4l2_captureparm expected = {
		.capability = V4L2_CAP_TIMEPERFRAME,
		.timeperframe = { 1, rate },
	};

	assert(!memcmp(&parm->parm.capture, &expected, sizeof(expected)));
}

static void ioctls(void)
{
	const unsigned int modes[] = { 0, 1, 30, 60 };
	const struct v4l2_fract times[] = {
		{ 1, 30 }, { 1, 60 }, { 2, 120 }, { 0, 60 }, { 1, 0 }, { 0, 0 },
		{ 1, 40 }, { 1, 41 }, { UINT32_MAX, UINT32_MAX }, { 1, UINT32_MAX },
	};
	unsigned int mode, i, busy;

	for (mode = 0; mode < ARRAY_SIZE(modes); mode++) {
		reset(720);
		cam_experiment_config.fps = modes[mode];
		for (busy = 0; busy < 2; busy++) {
			struct v4l2_streamparm parm = parameter(9, 999);
			unsigned int rate = modes[mode] == 60 ? 60 : 30;

			assert(!cam_parm(NULL, NULL, &parm));
			check_parm(&parm, rate);
			for (i = 0; i < 3; i++) {
				struct v4l2_frmivalenum value = interval(i);
				int ret = cam_enum_intervals(NULL, NULL, &value);

				if (i > (modes[mode] == 1 ? 1U : 0U)) {
					assert(ret == -EINVAL);
				} else {
					assert(!ret && value.type == V4L2_FRMIVAL_TYPE_DISCRETE);
					assert(value.discrete.numerator == 1);
					assert(value.discrete.denominator == (modes[mode] == 60 || i ? 60U : 30U));
				}
			}
			for (i = 0; i < ARRAY_SIZE(times); i++) {
				struct v4l2_streamparm before;

				parm = parameter(times[i].numerator, times[i].denominator);
				before = parm;
				/* Fixed modes report the latched interval, not the retuned one. */
				if (busy && modes[mode] == 1) {
					assert(cam_set_parm(NULL, NULL, &parm) == -EBUSY);
					assert(!memcmp(&before, &parm, sizeof(parm)));
				} else {
					if (modes[mode] == 1)
						rate = times[i].numerator && times[i].denominator &&
							(u64)times[i].numerator * 40 < times[i].denominator ? 60 : 30;
					assert(!cam_set_parm(NULL, NULL, &parm));
					check_parm(&parm, rate);
					parm = parameter(9, 999);
					assert(!cam_parm(NULL, NULL, &parm));
					check_parm(&parm, rate);
				}
			}
			if (!busy) {
				/* Start from the default selection for the streaming assertions. */
				cam.frame_rate = 0;
				assert(!cam_session_start(&cam));
				cam.queue.streaming = true;
				cam_experiment_config.fps = modes[mode] == 60 ? 0 : 60;
			}
		}
		cam_session_stop(&cam);
	}
	reset(720);
	cam_experiment_config.fps = 1;
	{
		struct v4l2_streamparm parm = parameter(1, 60);

		assert(!cam_set_parm(NULL, NULL, &parm) && !trace_count);
		assert(!cam_session_start(&cam) && cam.session.experiment.frame_rate == 60);
		cam.queue.streaming = true;
		parm = parameter(1, 30);
		assert(cam_set_parm(NULL, NULL, &parm) == -EBUSY && cam.frame_rate == 60);
		assert(!cam_parm(NULL, NULL, &parm));
		check_parm(&parm, 60);
		cam_session_stop(&cam);
	}
	for (mode = 0; mode < ARRAY_SIZE(modes); mode++) {
		struct v4l2_streamparm parm = parameter(1, 30), before;
		struct v4l2_frmivalenum value;

		reset(720);
		cam_experiment_config.fps = modes[mode];
		parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		before = parm;
		assert(cam_parm(NULL, NULL, &parm) == -EINVAL && !memcmp(&parm, &before, sizeof(parm)));
		assert(cam_set_parm(NULL, NULL, &parm) == -EINVAL && !memcmp(&parm, &before, sizeof(parm)));
		for (i = 0; i < 3; i++) {
			value = interval(0);
			if (!i)
				value.pixel_format = V4L2_PIX_FMT_YUYV;
			else if (i == 1)
				value.width++;
			else
				value.height++;
			assert(cam_enum_intervals(NULL, NULL, &value) == -EINVAL);
		}
	}
}

/* v4l2-compliance 1.32 interval and G/S_PARM checks, after the core's field clearing. */
static unsigned int compliance_intervals(u32 format, u32 width, u32 height, struct v4l2_fract *list)
{
	const size_t clear = offsetof(struct v4l2_frmivalenum, type);
	struct v4l2_frmivalenum value;
	unsigned int f;
	int ret;

	for (f = 0;; f++) {
		memset(&value, 0xff, sizeof(value));
		value.index = f;
		value.pixel_format = format;
		value.width = width;
		value.height = height;
		memset((u8 *)&value + clear, 0, sizeof(value) - clear);
		ret = cam_enum_intervals(NULL, NULL, &value);
		if (ret == -EINVAL)
			return f;
		assert(!ret && f < 2 && value.index == f && value.pixel_format == format);
		assert(value.width == width && value.height == height);
		assert(!value.reserved[0] && !value.reserved[1]);
		assert(value.type == V4L2_FRMIVAL_TYPE_DISCRETE);
		assert(value.discrete.numerator && value.discrete.denominator);
		list[f] = value.discrete;
	}
}

static void compliance_parm(const struct v4l2_streamparm *parm, const struct v4l2_fract *list,
			    unsigned int count)
{
	const struct v4l2_captureparm *cap = &parm->parm.capture;
	unsigned int i;

	assert(parm->type == V4L2_BUF_TYPE_VIDEO_CAPTURE);
	for (i = 0; i < ARRAY_SIZE(cap->reserved); i++)
		assert(!cap->reserved[i]);
	/* No READWRITE capability; intervals are enumerated, so TIMEPERFRAME is required. */
	assert(!cap->readbuffers && !cap->extendedmode && cap->capability == V4L2_CAP_TIMEPERFRAME);
	assert(!(cap->capturemode & ~V4L2_MODE_HIGHQUALITY));
	assert(cap->timeperframe.numerator && cap->timeperframe.denominator);
	/* Stricter than the tool: the reported interval is an enumerated one. */
	for (i = 0; i < count; i++)
		if (list[i].numerator == cap->timeperframe.numerator &&
		    list[i].denominator == cap->timeperframe.denominator)
			return;
	abort();
}

static void compliance(void)
{
	const unsigned int modes[] = { 0, 1, 30, 60 };
	unsigned int mode, busy, live, type, count;
	struct v4l2_fract list[2], unused[2];
	struct v4l2_streamparm parm, got;

	for (mode = 0; mode < ARRAY_SIZE(modes); mode++)
		for (busy = 0; busy < 2; busy++)
			for (live = 0; live < ARRAY_SIZE(modes); live++) {
				reset(720);
				cam.frame_rate = 60;
				cam_experiment_config.fps = modes[mode];
				if (busy) {
					assert(!cam_session_start(&cam));
					cam.queue.streaming = true;
				}
				/* Idle follows the parameter; a stream keeps its latched mode. */
				cam_experiment_config.fps = modes[busy ? live : mode];
				count = compliance_intervals(V4L2_PIX_FMT_NV12, CAM_FRAME_WIDTH,
							     CAM_DELIVER_HEIGHT, list);
				assert(count == (modes[mode] == 1 ? 2U : 1U));
				assert(!compliance_intervals(V4L2_PIX_FMT_NV12, CAM_FRAME_WIDTH + 1,
							     CAM_DELIVER_HEIGHT, unused));
				assert(!compliance_intervals(0x20202020, CAM_FRAME_WIDTH,
							     CAM_DELIVER_HEIGHT, unused));
				for (type = 0; type <= 0x80; type++) {
					memset(&parm, 0, sizeof(parm));
					parm.type = type;
					if (type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
						assert(cam_parm(NULL, NULL, &parm) == -EINVAL);
						assert(cam_set_parm(NULL, NULL, &parm) == -EINVAL);
						continue;
					}
					assert(!cam_parm(NULL, NULL, &parm));
					compliance_parm(&parm, list, count);
					/* The tool's zeroed S_PARM; follow mode refuses it mid-stream. */
					memset(&parm, 0, sizeof(parm));
					parm.type = type;
					if (busy && modes[mode] == 1) {
						assert(cam_set_parm(NULL, NULL, &parm) == -EBUSY);
						continue;
					}
					assert(!cam_set_parm(NULL, NULL, &parm));
					compliance_parm(&parm, list, count);
					memset(&got, 0, sizeof(got));
					got.type = type;
					assert(!cam_parm(NULL, NULL, &got) && !memcmp(&got, &parm, sizeof(got)));
				}
				if (busy)
					cam_session_stop(&cam);
			}
}
#endif

int main(void)
{
	(void)cam_experiment_uint_set;
	(void)cam_production_defaults;
	capture = malloc(CAM_CAPTURE_SIZE);
	assert(capture);
	validation();
	wire_and_order();
	reply_policy();
	pair_order();
	latch();
	ioctls();
	compliance();
	free(capture);
	puts("fps tests passed: validation, wire, fallback order, failures, latch and V4L2 compliance");
	return 0;
}
