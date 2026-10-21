// SPDX-License-Identifier: GPL-2.0-only
/* Retained colour and production sessions use synthetic firmware data only. */
#include "experiment_harness.h"
#include ".build/experiment_defaults.h"

static void write_replay(void)
{
	unsigned int i;
	u32 length;

	fwrite(&trace_count, sizeof(trace_count), 1, stdout);
	for (i = 0; i < trace_count; i++) {
		fwrite(&trace[i].endpoint, sizeof(trace[i].endpoint), 1, stdout);
		fwrite(&trace[i].size, sizeof(trace[i].size), 1, stdout);
		fwrite(&trace[i].argument, sizeof(trace[i].argument), 1, stdout);
		fwrite(trace[i].command, sizeof(command), 1, stdout);
	}
	fwrite(capture, cam.geometry.capture_size, 1, stdout);
	fwrite(command, sizeof(command), 1, stdout);
	fwrite(&record, sizeof(record), 1, stdout);
	strip_pacing_log();
	length = strlen(logs);
	assert(length < sizeof(logs) - 1);
	fwrite(&length, sizeof(length), 1, stdout);
	fwrite(logs, length, 1, stdout);
}

static void production_trace(bool output, bool tail)
{
	unsigned int height, colour, session;

	for (height = 720; height <= 1264; height += 544)
		for (colour = 0; colour <= 1; colour++)
			for (session = 0; session < 2; session++) {
				reset(height);
				cam.capture_span = tail ? CAM_CAPTURE_MAX : CAM_CAPTURE_SIZE;
				cam.fw_sessions = session;
				cam_experiment_config = cam_production_defaults;
				cam_experiment_config.colour = colour;
				assert(!cam_session_start(&cam));
				run_stream();
				assert(!get_unaligned_le32(trace[index_opcode(0, 0)].command + 8));
				if (output)
					write_replay();
			}
}

static void highres_trace(bool output)
{
	unsigned int colour, session, rate;

	for (colour = 0; colour <= 1; colour++)
		for (session = 0; session < 2; session++)
			for (rate = 30; rate <= 60; rate += 30) {
				reset(1888);
				cam.fw_sessions = session;
				cam.frame_rate = rate;
				cam_experiment_config = cam_production_defaults;
				cam_experiment_config.colour = colour;
				assert(!cam_session_start(&cam));
				run_stream();
				if (output)
					write_replay();
			}
}

int main(int argc, char **argv)
{
	(void)cam_experiment_uint_set;
	(void)count_opcode;
	capture = malloc(CAM_CAPTURE_MAX);
	assert(capture);
	if (argc == 2) {
		if (!strcmp(argv[1], "--1080p-trace"))
			highres_trace(true);
		else if (!strcmp(argv[1], "--production-trace-no-tail"))
			production_trace(true, false);
		else {
			assert(!strcmp(argv[1], "--production-trace"));
			production_trace(true, true);
		}
	} else {
		assert(argc == 1);
		production_trace(false, true);
		production_trace(false, false);
		highres_trace(false);
		puts("sequence tests passed: 8 production sessions with and without the tail, 8 1080p sessions, requests, pool, IPC and logs");
	}
	free(capture);
	return 0;
}
