// SPDX-License-Identifier: GPL-2.0-only
/* Missing owner data, rejected colour and framing requests and zoom drift stay bounded. */
#include "experiment_harness.h"
#include ".build/experiment_defaults.h"

static void defaults(void)
{
	reset(1264);
	cam_experiment_config = cam_production_defaults;
}

static void invalid_data(void)
{
	unsigned int kind;

	for (kind = 0; kind < 5; kind++) {
		defaults();
		if (kind == 0)
			request_error = -ENOENT;
		else if (kind == 1)
			blob.size--;
		else if (kind == 2)
			blob_data[0] ^= 1;
		else if (kind == 3)
			put_unaligned_le32(0x21, blob_data + 0x10);
		else
			put_unaligned_le32(0x959, blob_data + 0x6fd4);
		assert(!cam_session_start(&cam) && !cam_dead(&cam));
		assert(!count_opcode(0x50f) && !count_opcode(0x519) && !count_opcode(0x111));
		assert(cam_setfile_result == -EINVAL && cam_lenscal_result == -EINVAL);
		run_stream();
		assert(requests == 1 && releases == (kind != 0));
	}
}

static void colour_failures(void)
{
	unsigned int sites[5], i, fault;

	defaults();
	assert(!cam_session_start(&cam));
	sites[0] = index_opcode(0x50f, 0) + 1;
	sites[1] = index_opcode(0x50f, 1) + 1;
	sites[2] = index_opcode(0x519, 0) + 1;
	sites[3] = index_opcode(0x519, 1) + 1;
	sites[4] = index_opcode(0x111, 0) + 1;
	run_stream();
	for (i = 0; i < ARRAY_SIZE(sites); i++)
		for (fault = 0; fault < 2; fault++) {
			defaults();
			if (fault) {
				timeout_at = sites[i];
				assert(cam_session_start(&cam) == -EIO && cam_dead(&cam));
				assert(trace_count == sites[i] && cam.query_stalled);
			} else {
				reject_at = sites[i];
				assert(!cam_session_start(&cam) && !cam_dead(&cam));
				assert(cam_setfile_result == -EPROTO || cam_lenscal_result == -EPROTO);
				assert(count_opcode(0x111) == (i == 4));
				run_stream();
			}
			assert(releases == 1);
		}
}

static void latch_and_zoom(void)
{
	struct kernel_param colour = { &cam_experiment_config.colour };
	struct kernel_param framing = { &cam_experiment_config.framing };

	defaults();
	assert(cam_experiment_uint_set("2", &colour) == -EINVAL);
	assert(cam_experiment_config.colour == 1);
	assert(cam_experiment_uint_set("3", &framing) == -EINVAL);
	assert(cam_experiment_config.framing == 2);
	retune_at_request = true;
	assert(!cam_session_start(&cam));
	assert(cam.session.experiment.config.colour == 1);
	assert(cam.session.experiment.config.framing == 2 && cam.session.experiment.frame_rate == 30);
	assert(!cam_experiment_config.colour && !cam_experiment_config.framing);
	assert(!cam_experiment_uint_set("1\n", &colour));
	run_stream();
	defaults();
	stream_limit = 92;
	drift_at_requeue = 2;
	assert(!cam_session_start(&cam));
	run_stream();
	assert(count_opcode(0x810) == 2 && count_opcode(0x811) >= 4);
	assert(strstr(logs, "framing periodic: re-assert zoom=0x400"));
	assert(!cam_dead(&cam) && releases == 1);
}

static void production(unsigned int height)
{
	reset(height);
	cam_experiment_config = cam_production_defaults;
}

static unsigned int count_logs(const char *needle)
{
	const char *line = logs;
	unsigned int count = 0;

	while ((line = strstr(line, needle))) {
		count++;
		line++;
	}
	return count;
}

/* framing=2 is on by default: a rejected S3.5 reply turns framing off for that
 * session only, a missing reply is fatal, and the colour setfile is unaffected.
 */
static void framing_setup_failures(void)
{
	unsigned int colour, step, outcome, at;

	for (colour = 0; colour <= 1; colour++)
		for (step = 0; step < 4; step++)
			for (outcome = 0; outcome < 3; outcome++) {
				production(1264);
				cam_experiment_config.colour = colour;
				assert(!cam_session_start(&cam));
				at = index_opcode(0x800, 0) + 1 + step;
				cam_session_stop(&cam);
				production(1264);
				cam_experiment_config.colour = colour;
				reject_status = outcome ? 0xffff : 0xdead;
				reject_at = outcome < 2 ? at : 0;
				timeout_at = outcome == 2 ? at : 0;
				assert(cam_session_start(&cam) == (outcome == 2 ? -EIO : 0));
				assert(framing_sends == step + 1);
				assert(count_opcode(0x111) == colour);
				if (outcome < 2) {
					assert(!cam_dead(&cam) && !cam.session.experiment.framing_enabled);
					assert(strstr(logs, "framing disabled for session: result=-71"));
					run_stream();
					assert(cam.session.requeues == stream_limit && !cam_dead(&cam));
				} else {
					assert(cam_dead(&cam) && cam.query_stalled && failures == 1);
					assert(strstr(logs, "framing disabled for session: result=-110"));
				}
				assert(framing_sends == step + 1 && !cam_setfile_result && !cam_lenscal_result);
			}
}

/* The S5 readback, its re-assert, the periodic readback and its re-assert. */
static void framing_stream_failures(void)
{
	unsigned int site, outcome, at, before;

	for (site = 0; site < 4; site++)
		for (outcome = 0; outcome < 3; outcome++) {
			production(720);
			assert(!cam_session_start(&cam));
			before = framing_sends;
			drift_at_requeue = site < 2 ? 1 : 2;
			at = trace_count + (site < 2 ? 2 + site : 33 + site - 2);
			reject_status = outcome ? 0xffff : 0xdead;
			reject_at = outcome < 2 ? at : 0;
			timeout_at = outcome == 2 ? at : 0;
			stream_limit = 33;
			run_stream();
			assert(framing_sends == before + (site < 2 ? site + 1 : site));
			assert(cam_dead(&cam) == (outcome == 2) && failures == (outcome == 2));
			assert(cam.session.requeues == (outcome == 2 ? (site < 2 ? 1U : 31U) : stream_limit));
			assert(!cam_setfile_result && !cam_lenscal_result && releases == 1);
			/* A failed re-assert still logs the drift that triggered it. */
			if (site & 1)
				assert(strstr(logs, site == 1 ?
					      "framing S5: re-assert zoom=0x400 scaler0=0x600 scaler1=0x401" :
					      "framing periodic: re-assert zoom=0x400 scaler0=0x600 scaler1=0x401"));
		}
}

static void pin_session(void)
{
	production(1264);
	cam.fw_sessions = 1;
	cam_experiment_begin(&cam);
	assert(cam.session.experiment.framing_enabled);
	assert(!cam_framing_start(&cam));
	cam.session.requeues = 1;
	cam.session.delivered = 2;
}

static void framing_dead_and_cadence(void)
{
	u16 zoom[2];
	unsigned int step, before, i;

	reset(720);
	cam.state = CAM_DEAD;
	assert(cam_framing_crop(&cam, "dead") == -EIO);
	assert(cam_framing_zoom(&cam, "dead", zoom) == -EIO);
	assert(cam_framing_pin(&cam) == -EIO && !trace_count);
	/* DEAD before or after each S3.5 send stops every later framing send. */
	for (step = 0; step <= 4; step++) {
		production(1264);
		cam.fw_sessions = 1;
		cam_experiment_begin(&cam);
		if (!step)
			cam.state = CAM_DEAD;
		else
			dead_after_send = step;
		assert(!cam_framing_start(&cam));
		assert(trace_count == step && cam_dead(&cam));
		before = trace_count;
		assert(!cam_framing_requeue(&cam) && trace_count == before);
		cam_experiment_end(&cam);
	}
	for (step = 1; step <= 2; step++) {
		pin_session();
		zoom_reply[0] = 0x500;
		before = trace_count;
		dead_after_send = before + step;
		assert(!cam_framing_requeue(&cam));
		assert(trace_count == before + step && cam_dead(&cam));
		cam_experiment_end(&cam);
	}
	/* The 60-delivered-frame poll survives the counter wrapping. */
	pin_session();
	assert(!cam_framing_requeue(&cam));
	cam.session.experiment.framing_delivered = UINT32_MAX - 29;
	cam.session.delivered = 29;
	before = trace_count;
	assert(!cam_framing_requeue(&cam) && trace_count == before);
	cam.session.delivered = 30;
	assert(!cam_framing_requeue(&cam) && trace_count == before + 1);
	cam_experiment_end(&cam);
	/* Persistent drift re-asserts at every poll but logs ten lines a minute. */
	pin_session();
	for (i = 0; i < 26; i++) {
		jiffies += i == 25 ? 60 * HZ : HZ;
		zoom_reply[0] = 0x600;
		zoom_reply[1] = 0x400;
		cam.session.delivered = 2 + 60 * i;
		assert(!cam_framing_requeue(&cam));
	}
	assert(count_logs("framing periodic: re-assert zoom=0x400 scaler0=0x600 scaler1=0x400") == 10);
	assert(limited_logs == 26 && suppressed_logs == 15 && !cam_dead(&cam));
	cam_experiment_end(&cam);
}

int main(void)
{
	capture = malloc(CAM_CAPTURE_SIZE);
	assert(capture);
	invalid_data();
	colour_failures();
	latch_and_zoom();
	framing_setup_failures();
	framing_stream_failures();
	framing_dead_and_cadence();
	free(capture);
	puts("production tests passed: owner data, colour/framing faults, DEAD guards, latch, cadence, drift");
	return 0;
}
