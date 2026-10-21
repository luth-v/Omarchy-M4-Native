// SPDX-License-Identifier: GPL-2.0-only
/* Production session, output gate and format callbacks, synthetic data only. */
#include <linux/videodev2.h>

#include "experiment_harness.h"
#include ".build/claim_constants.h"

struct file { int unused; };
struct device { int unused; };
struct vb2_buffer { struct vb2_queue *vb2_queue; size_t size, payload; void *data; };
static struct cam_device *test_device(void)
{
	return &cam;
}
#define video_drvdata(file) ((void)(file), test_device())
#define vb2_get_drv_priv(queue) ((void)(queue), test_device())
#define vb2_is_streaming(queue) ((queue)->streaming)
#define vb2_is_busy(queue) ((queue)->busy || (queue)->streaming)
#define vb2_plane_size(buffer, plane) ((void)(plane), (buffer)->size)
#define vb2_plane_vaddr(buffer, plane) ((void)(plane), (buffer)->data)
#define vb2_set_plane_payload(buffer, plane, size) ((void)(plane), (buffer)->payload = (size))
#include ".build/fps_params_impl.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include ".build/format_impl.h"
#include ".build/fps_v4l2_impl.h"
#pragma GCC diagnostic pop

static void geometry(void)
{
	struct cam_geometry g;
	struct cam_buffer buffer = { 0 };
	unsigned int slot, index;
	u64 base = 0x10040000000ULL;

	assert(cam_geometry_init(&g, 1888));
	assert(g.width == 1920 && g.height == 1888 && g.deliver_height == 1080);
	assert(g.y_size == 0x375000 && g.uv_size == 0x1ba800);
	assert(g.y_span == 0x378000 && g.uv_span == 0x1bc000 && g.frame_stride == 0x534000);
	assert(g.pool_end == 0xda8000 && g.capture_size == 0xdc0000);
	/* 82f9fd25's claim stays the base size; the 1080p claim adds 5.75 MiB. */
	assert(CAM_IDENTITY_SIZE == 0x23200000 && CAM_IDENTITY_MAX == CAM_IDENTITY_SIZE + 0x5c0000);
	assert(g.capture_size + CAM_EXTRA_SIZE + CAM_ARENA_SIZE == CAM_IDENTITY_MAX);
	assert(CAM_ALIGN16K(g.pool_end) + CAM_SETFILE_MAX == 0xdb8000);
	assert(g.deliver_y == 2073600 && g.deliver_size == 3110400);
	assert(g.crop_y == 404 && g.crop_uv == 202);
	assert(g.crop_y * 2 + g.deliver_height == g.height);
	assert(g.crop_uv * 2 + g.deliver_height / 2 == g.height / 2);
	for (slot = 0; slot < 2; slot++) {
		u64 y = base + 0x340000 + slot * 0x534000;

		buffer.type = cpu_to_le32(1);
		buffer.planes = cpu_to_le32(2);
		buffer.iova[0] = cpu_to_le64(y);
		buffer.iova[1] = cpu_to_le64(y + 0x378000);
		assert(cam_buffer_index(&g, base, &buffer, &index) && index == slot);
		buffer.iova[1] = cpu_to_le64(y + 0x375000);
		assert(!cam_buffer_index(&g, base, &buffer, &index));
	}
	buffer.iova[0] = cpu_to_le64(base + 0xda8000);
	buffer.iova[1] = cpu_to_le64(base + 0xda8000 + 0x378000);
	assert(!cam_buffer_index(&g, base, &buffer, &index));
	assert(cam_geometry_init(&g, 1264) && g.capture_size == 0x800000);
	assert(g.pool_end == 0x7e8000 && g.frame_stride == 0x254000);
	assert(cam_geometry_init(&g, 720) && g.capture_size == 0x800000);
	assert(g.pool_end == 0x5f0000 && g.frame_stride == 0x158000);
}

static void output_record(struct cam_output_evidence *e, const char *text, u16 kind, u16 lost)
{
	struct cam_print_record message = { .kind = cpu_to_le16(kind), .dropped = cpu_to_le16(lost) };

	snprintf(message.text, sizeof(message.text), "%s", text);
	cam_output_record(e, &message);
}

static void evidence(void)
{
	/* Verbatim from the owner's 2026-10-16 logs, then with the 1080p values. */
	const char *logged = "[MSC] CH = 0x0 BES[0] originalOutputSize [1280 720]\n";
	const char *valid = "[MSC] CH = 0x0 BES[0] originalOutputSize [1920 1888]\n";
	const char *replaced = "MaxFESOutput was (3072, 3024), Replaced with (1920, 1080)\n";
	struct cam_output_evidence e = { 0 };
	const char * const invalid[] = {
		logged,
		"[MSC] CH = 0x0 BES[0] originalOutputSize [1280 1264]\n",
		"[MSC] CH = 0x0 BES[0] originalOutputSize [1920 1080]\n",
		"[MSC] CH = 0x0 BES[0] originalOutputSize [1888 1920]\n",
		"[MSC] CH = 0x0 BES[0] originalOutputSize [1920 1888], actual [1920 1080]\n",
		"[MSC] CH = 0x0 BES[0] originalOutputSize [1920 1888], actual [1920]\n",
	};
	/* Never evidence: the unprinted format, other channels/outputs and other lines. */
	const char * const ignored[] = {
		"ch=0,BESType=1,outFmt=0,w=1920,h=1888,refW=1920,refH=1888,paddingRows=0,cmp=0",
		"[MSC] CH = 0x1 BES[0] originalOutputSize [1920 1888]\n",
		"[MSC] CH = 0x0 BES[1] originalOutputSize [1920 1888]\n",
		"[MSC] CH = 0x0, cfg=0, CmdSyncTag=0\n",
		"[MSC] CH = 0x0 BES[0] originalOutputSize [1920\n",
		"GetMaxFESOutputSize():W=3072,H=3024,bHasNoneSyncCh=1,activeChMask=0x0,activeChCnt=0\n",
	};
	unsigned int i;

	output_record(&e, valid, 0, 0);
	assert(!e.seen && !e.rejected);
	e.watching = true;
	output_record(&e, valid, 1, 0);
	for (i = 0; i < ARRAY_SIZE(ignored); i++)
		output_record(&e, ignored[i], 0, 0);
	assert(!e.seen && !e.rejected);
	output_record(&e, valid, 0, 0);
	assert(e.seen && !e.rejected && e.width == 1920 && e.height == 1888);
	output_record(&e, "[MSC] CH = 0x0 BES[0] originalOutputSize [1920 1888], actual [1920 1888]\n",
		      0, 0);
	assert(e.seen && !e.rejected);
	output_record(&e, replaced, 0, 0);
	output_record(&e, valid, 0, 0);
	assert(e.seen && e.rejected && e.replaced && !e.dropped);
	for (i = 0; i < ARRAY_SIZE(invalid); i++) {
		e = (struct cam_output_evidence) { .watching = true };
		output_record(&e, invalid[i], 0, 0);
		assert(e.seen && e.rejected && !e.replaced && !e.dropped);
	}
	/* A lost record rejects until the gate accepts; a replacement always does. */
	e = (struct cam_output_evidence) { .watching = true };
	output_record(&e, valid, 0, 1);
	assert(e.seen && e.rejected && e.dropped);
	e = (struct cam_output_evidence) { .watching = true };
	output_record(&e, "other", 2, 3);
	assert(!e.seen && e.rejected && e.dropped);
	e = (struct cam_output_evidence) { .watching = true, .accepted = true };
	output_record(&e, valid, 0, 1);
	output_record(&e, "other", 2, 3);
	assert(e.seen && !e.rejected && !e.dropped);
	output_record(&e, replaced, 0, 0);
	assert(e.rejected && e.replaced && !e.dropped);
}

static void wire(void)
{
	u8 expected[CAM_QUERY_CAPACITY] = { 0 };
	struct trace *t = &trace[index_opcode(0xb01, 0)];
	unsigned int i, j;
	const u16 pools[] = { 0, 8, 1 };

	put_unaligned_le16(0xb01, expected + 4);
	put_unaligned_le32(1920, expected + 0xc);
	put_unaligned_le32(1888, expected + 0x10);
	put_unaligned_le32(1, expected + 0x14);
	put_unaligned_le32(1920, expected + 0x1c);
	put_unaligned_le32(1920, expected + 0x20);
	put_unaligned_le32(1888, expected + 0x2c);
	put_unaligned_le32(1920, expected + 0x34);
	assert(t->size == 0x38 && !t->argument && !memcmp(t->command, expected, sizeof(expected)));
	for (i = 0; i < ARRAY_SIZE(pools); i++) {
		t = &trace[index_opcode(0x117, i)];
		memset(expected, 0, sizeof(expected));
		put_unaligned_le16(0x117, expected + 4);
		put_unaligned_le16(pools[i], expected + 0xc);
		put_unaligned_le16(16, expected + 0xe);
		put_unaligned_le32(i == 2 ? 0x375000 : i == 1 ? 0x200 : 0x100, expected + 0x10);
		put_unaligned_le32(i == 2 ? 1920 : i == 1 ? 0x200 : 0x100, expected + 0x14);
		if (i == 2) {
			put_unaligned_le32(0x1ba800, expected + 0x30);
			put_unaligned_le32(1920, expected + 0x34);
		}
		put_unaligned_le16(i == 2 ? 2 : 1, expected + 0x94);
		assert(t->size == 0x9c && t->argument == 0x9c);
		assert(!memcmp(t->command, expected, sizeof(expected)));
	}
	assert(!get_unaligned_le32(trace[index_opcode(0x107, 0)].command + 0xc));
	assert(!count_opcode(0x800) && !count_opcode(0x80a));
	assert(!capture[0xdb8000] && !capture[0xdc0000 - 1]);
	for (i = 0; i < trace_count; i++) {
		struct cam_buffer_list *list;

		t = &trace[i];
		if (t->endpoint != 0x24)
			continue;
		list = (struct cam_buffer_list *)t->command;
		for (j = 0; j < le64_to_cpu(list->count); j++) {
			struct cam_buffer *b = &list->buffers[j];
			u64 y = le64_to_cpu(b->iova[0]);
			unsigned int slot;

			if (le32_to_cpu(b->type) != 1)
				continue;
			assert(y == cam.capture_base + 0x340000 || y == cam.capture_base + 0x874000);
			assert(le64_to_cpu(b->iova[1]) == y + 0x378000);
			assert(cam_buffer_index(&cam.geometry, cam.capture_base, b, &slot));
		}
	}
}

/* At most one clamp line per session, and only where 60 was asked for. */
static unsigned int clamp_lines(void)
{
	const char *clamp = "fps: 60 not offered at 1080p, using 30\n", *line = logs;
	unsigned int count = 0;

	while ((line = strstr(line, clamp))) {
		count++;
		line++;
	}
	return count;
}

static void sessions(void)
{
	const unsigned int modes[] = { 0, 1, 30, 60 };
	struct v4l2_streamparm parm = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	struct v4l2_frmivalenum interval = { .pixel_format = V4L2_PIX_FMT_NV12,
					       .width = 1920, .height = 1080 };
	unsigned int colour, framing, mode, firmware_session, asked;

	for (colour = 0; colour < 2; colour++)
		for (framing = 0; framing < 3; framing++)
			for (mode = 0; mode < ARRAY_SIZE(modes); mode++)
				for (firmware_session = 0; firmware_session < 2; firmware_session++) {
					reset(1888);
					cam.fw_sessions = firmware_session;
					cam.frame_rate = 60;
					cam_experiment_config = (struct cam_experiment_config) {
						.colour = colour, .framing = framing, .fps = modes[mode],
					};
					assert(!cam_session_start(&cam) && !cam_dead(&cam));
					assert(cam.session.output.seen && !cam.session.output_rejected);
					assert(cam.session.output.accepted && !cam.session.output.rejected);
					assert(strstr(logs, "1080p: firmware output accepted 1920x1888\n"));
					/* 1080p offers 30 only: a requested 60 pins 30. */
					asked = modes[mode] == 1 ? 60 : modes[mode];
					assert(cam.session.experiment.frame_rate == (asked ? 30 : 0));
					assert(clamp_lines() == (asked == 60));
					assert(count_opcode(0x260) == !!asked);
					if (asked) {
						const u8 *pin = trace[index_opcode(0x260, 0)].command;

						assert(get_unaligned_le32(pin + 0x10) == 30);
						assert(get_unaligned_le32(pin + 0x18) == 30);
						assert(strstr(logs, "fps: requested=30 pin_result=0 "));
					}
					/* While streaming: G_PARM 1/30, one 1/30 interval, fixed S_PARM 1/30. */
					cam.queue.streaming = true;
					assert(!cam_parm(NULL, NULL, &parm));
					assert(parm.parm.capture.timeperframe.denominator == 30);
					interval.index = 0;
					assert(!cam_enum_intervals(NULL, NULL, &interval));
					assert(interval.discrete.denominator == 30);
					interval.index = 1;
					assert(cam_enum_intervals(NULL, NULL, &interval) == -EINVAL);
					parm.parm.capture.timeperframe = (struct v4l2_fract) { 1, 60 };
					assert(cam_set_parm(NULL, NULL, &parm) == (modes[mode] == 1 ? -EBUSY : 0));
					assert(modes[mode] == 1 || parm.parm.capture.timeperframe.denominator == 30);
					cam.queue.streaming = false;
					assert(count_opcode(0x810) == (framing == 2 && !firmware_session));
					wire();
					run_stream();
					assert(cam.session.delivered == 6 && !cam.session.dropped && !cam_dead(&cam));
					assert(strstr(logs, "copy_avg=") && strstr(logs, " pacing:"));
					assert(clamp_lines() == (asked == 60));
					wire();
				}
	/* Complete production sequence, including list endpoints, before rendering. */
	{
		const u16 expected[] = {
			0x3206, 0, 0x10d, 0x50f, 0x50f, 0x519, 0x24, 0x13b, 0x107, 0x105,
			0x10e, 0x10f, 0xb01, 0x117, 0x117, 0x117, 0x125, 0xb0d, 0xffff,
			0x110, 0x100, 0x519, 0x111, 0x811, 0x810, 0x811, 0x260, 0x207, 0x209, 0xffff,
		};
		unsigned int i;

		reset(1888);
		cam_experiment_config = (struct cam_experiment_config) { .colour = 1, .framing = 2, .fps = 1 };
		assert(!cam_session_start(&cam) && trace_count == ARRAY_SIZE(expected));
		assert(cam.session.experiment.frame_rate == 30);
		for (i = 0; i < trace_count; i++) {
			assert(trace[i].endpoint == (expected[i] == 0xffff ? 0x24 : 0x21));
			assert(expected[i] == 0xffff || get_unaligned_le16(trace[i].command + 4) == expected[i]);
		}
		run_stream();
	}
}

static void gate(void)
{
	/* 1 none, 2 wrong size, 3 replaced, 4 replaced at S3.5, 5 after capture, 6 lost. */
	const char * const reasons[] = {
		NULL,
		"rejected at 0xb01: result=0 seen=0 size=0x0 replaced=0 dropped=0\n",
		"rejected at 0xb01: result=0 seen=1 size=1920x1080 replaced=0 dropped=0\n",
		"rejected at 0xb01: result=0 seen=1 size=1920x1888 replaced=1 dropped=0\n",
		"rejected before capture: result=0 seen=1 size=1920x1888 replaced=1 dropped=0\n",
		"rejected after the capture list: result=0 seen=1 size=1920x1888 replaced=1 dropped=0\n",
		"rejected at 0xb01: result=0 seen=1 size=1920x1888 replaced=0 dropped=1\n",
	};
	unsigned int model, output;

	for (model = 1; model < ARRAY_SIZE(reasons); model++) {
		bool late = model == 4 || model == 5;

		reset(1888);
		output_model = model;
		cam_experiment_config.fps = 1;
		assert(cam_session_start(&cam) == -EIO && !cam_dead(&cam));
		assert(cam.session.output_rejected && cam.session.stopped && !cam.thread);
		assert(!cam.session.delivered && !failures && !dumps && !cam.session.output.accepted);
		assert(strstr(logs, reasons[model]) && !strstr(logs, "accepted"));
		assert(count_opcode(1) == 1 && count_opcode(0x1b) == 1);
		assert(count_opcode(0x100) == late && count_opcode(0x101) == late);
		assert(count_opcode(0x117) == (late ? 3 : 0));
		assert(cam.session.capture_issued == (model == 5));
	}
	reset(1888);
	assert(!cam_session_start(&cam));
	output = index_opcode(0xb01, 0) + 1;
	reset(1888);
	reject_at = output;
	assert(cam_session_start(&cam) == -EIO && !cam_dead(&cam));
	assert(cam.session.output_rejected && cam.session.stopped && !count_opcode(0x117));
	assert(strstr(logs, "rejected at 0xb01: result=-71 seen=1 size=1920x1888"));
	reset(1888);
	timeout_at = output;
	assert(cam_session_start(&cam) == -EIO && cam_dead(&cam));
	assert(!cam.session.output_rejected && !count_opcode(0x117));
	/* A previous session's positive evidence cannot authorize the next. */
	reset(1888);
	assert(!cam_session_start(&cam));
	run_stream();
	output_model = 1;
	assert(cam_session_start(&cam) == -EIO && cam.session.output_rejected && !cam_dead(&cam));
}

static void formats(void)
{
	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	struct v4l2_frmsizeenum size = { .pixel_format = V4L2_PIX_FMT_NV12 };
	struct v4l2_frmivalenum interval = { .pixel_format = V4L2_PIX_FMT_NV12 };
	struct v4l2_streamparm parm = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	struct vb2_buffer buffer = { .vb2_queue = &cam.queue, .data = &fmt };
	unsigned int planes = 0, bytes[1], i, mode;
	const unsigned int modes[] = { 0, 1, 30, 60 };

	reset(1264);
	assert(!cam_get_fmt(NULL, NULL, &fmt) && fmt.fmt.pix.width == 1280);
	assert(fmt.fmt.pix.height == 720 && fmt.fmt.pix.sizeimage == 1382400);
	for (i = 0; i < 2; i++) {
		size.index = i;
		assert(!cam_enum_sizes(NULL, NULL, &size));
		assert(size.type == V4L2_FRMSIZE_TYPE_DISCRETE);
		assert(size.discrete.width == (i ? 1920 : 1280));
		assert(size.discrete.height == (i ? 1080 : 720));
		interval.width = size.discrete.width;
		interval.height = size.discrete.height;
		for (mode = 0; mode < ARRAY_SIZE(modes); mode++) {
			cam_experiment_config.fps = modes[mode];
			interval.index = 0;
			assert(!cam_enum_intervals(NULL, NULL, &interval));
			assert(interval.discrete.denominator == (!i && modes[mode] == 60 ? 60 : 30));
			interval.index = 1;
			assert(cam_enum_intervals(NULL, NULL, &interval) ==
			       (!i && modes[mode] == 1 ? 0 : -EINVAL));
			if (!i && modes[mode] == 1)
				assert(interval.discrete.denominator == 60);
			interval.index = 2;
			assert(cam_enum_intervals(NULL, NULL, &interval) == -EINVAL);
		}
	}
	size.index = 2;
	assert(cam_enum_sizes(NULL, NULL, &size) == -EINVAL);
	size.index = 0;
	size.pixel_format = V4L2_PIX_FMT_YUYV;
	assert(cam_enum_sizes(NULL, NULL, &size) == -EINVAL);
	fmt.fmt.pix.width = 1920;
	fmt.fmt.pix.height = 1080;
	assert(!cam_try_fmt(NULL, NULL, &fmt) && cam.format.width == 1280);
	assert(fmt.fmt.pix.bytesperline == 1920 && fmt.fmt.pix.sizeimage == 3110400);
	assert(!cam_set_fmt(NULL, NULL, &fmt) && cam.format.height == 1888);
	assert(cam.geometry.width == 1280);
	assert(!cam_get_fmt(NULL, NULL, &fmt) && fmt.fmt.pix.height == 1080);
	/* Idle at 1080p, S_PARM and G_PARM report 1/30 in every mode. A follow-mode
	 * 60 selection is kept and applies again at 720p.
	 */
	for (mode = 0; mode < ARRAY_SIZE(modes); mode++) {
		cam_experiment_config.fps = modes[mode];
		cam.frame_rate = 30;
		parm.parm.capture.timeperframe = (struct v4l2_fract) { 1, 60 };
		assert(!cam_set_parm(NULL, NULL, &parm));
		assert(parm.parm.capture.timeperframe.denominator == 30);
		assert(!cam_parm(NULL, NULL, &parm));
		assert(parm.parm.capture.timeperframe.denominator == 30);
		assert(cam.frame_rate == (modes[mode] == 1 ? 60 : 30));
	}
	cam_experiment_config.fps = 1;
	cam.frame_rate = 60;
	assert(!cam_queue_setup(&cam.queue, NULL, &planes, bytes, NULL) && planes == 1);
	assert(bytes[0] == 3110400);
	buffer.size = bytes[0] - 1;
	assert(cam_buffer_prepare(&buffer) == -EINVAL);
	buffer.size++;
	assert(!cam_buffer_prepare(&buffer) && buffer.payload == 3110400);
	buffer.data = NULL;
	assert(cam_buffer_prepare(&buffer) == -EINVAL);
	bytes[0]--;
	assert(cam_queue_setup(&cam.queue, NULL, &planes, bytes, NULL) == -EINVAL);
	cam.queue.busy = true;
	assert(cam_set_fmt(NULL, NULL, &fmt) == -EBUSY);
	cam.queue.busy = false;
	cam.queue.streaming = true;
	assert(cam_set_fmt(NULL, NULL, &fmt) == -EBUSY && cam.format.width == 1920);
	cam.queue.streaming = false;
	fmt.fmt.pix.width = 1280;
	fmt.fmt.pix.height = 720;
	assert(!cam_set_fmt(NULL, NULL, &fmt) && cam.format.height == 1264);
	assert(!cam_parm(NULL, NULL, &parm) && parm.parm.capture.timeperframe.denominator == 60);
	cam_experiment_config.fps = 60;
	assert(!cam_parm(NULL, NULL, &parm) && parm.parm.capture.timeperframe.denominator == 60);
	cam_experiment_config.fps = 0;
	cam.out_height = 720;
	assert(!cam_set_fmt(NULL, NULL, &fmt) && cam.format.height == 720);
	/* Without the 1080p tail only 1280x720 exists, and requests normalize to it. */
	reset(1264);
	cam.capture_span = CAM_CAPTURE_SIZE;
	size = (struct v4l2_frmsizeenum) { .pixel_format = V4L2_PIX_FMT_NV12 };
	assert(!cam_enum_sizes(NULL, NULL, &size) && size.discrete.width == 1280);
	size.index = 1;
	assert(cam_enum_sizes(NULL, NULL, &size) == -EINVAL);
	interval = (struct v4l2_frmivalenum) { .pixel_format = V4L2_PIX_FMT_NV12,
					       .width = 1920, .height = 1080 };
	assert(cam_enum_intervals(NULL, NULL, &interval) == -EINVAL);
	interval.width = 1280;
	interval.height = 720;
	assert(!cam_enum_intervals(NULL, NULL, &interval));
	fmt.fmt.pix.width = 1920;
	fmt.fmt.pix.height = 1080;
	assert(!cam_try_fmt(NULL, NULL, &fmt) && fmt.fmt.pix.width == 1280);
	assert(fmt.fmt.pix.height == 720 && fmt.fmt.pix.sizeimage == 1382400);
	fmt.fmt.pix.width = 1920;
	fmt.fmt.pix.height = 1080;
	assert(!cam_set_fmt(NULL, NULL, &fmt) && fmt.fmt.pix.height == 720);
	assert(cam.format.width == 1280 && cam.format.height == 1264);
	assert(cam.format.capture_size == CAM_CAPTURE_SIZE);
	assert(!trace_count);
	(void)cam_fps_set;
}

int main(void)
{
	(void)cam_experiment_uint_set;
	(void)strip_pacing_log;
	capture = malloc(CAM_CAPTURE_MAX);
	assert(capture);
	geometry();
	evidence();
	sessions();
	gate();
	formats();
	free(capture);
	puts("1080p tests passed: 48 session sequences, exact output/pool/list bytes, crop geometry, logged-print acceptance and replacement/mismatch/missing/lost/stale evidence, normal rejection stop, format/queue/interval callbacks with and without the 1080p claim, 30-only 1080p intervals, G/S_PARM and pin");
	return 0;
}
