// SPDX-License-Identifier: GPL-2.0-only
#include "j713_cam.h"

/* The lock makes the session settings read atomic
 * with respect to parameter writes. It is never held across a firmware wait.
 */
static DEFINE_MUTEX(cam_experiment_lock);
/* Production defaults, validated on hardware on 2026-10-18: the canonical
 * setfile with lens shading disabled, zoom pinned to 1x and the V4L2-selected
 * rate (30 fps unless an app asks for 60). A plain load, as at boot, uses them;
 * a session latches them exactly as it latches the same values written later.
 */
static struct cam_experiment_config cam_experiment_config = {
	.framing = 2,
	.colour = 1,
	.fps = 1,
};
int cam_setfile_result, cam_lenscal_result;
module_param_named(setfile_result, cam_setfile_result, int, 0444);
module_param_named(lenscal_result, cam_lenscal_result, int, 0444);

static int cam_experiment_uint_set(const char *value, const struct kernel_param *parameter)
{
	size_t length = strlen(value);
	char maximum = parameter->arg == &cam_experiment_config.colour ? '1' : '2';

	if (length && value[length - 1] == '\n')
		length--;
	if (length != 1 || value[0] < '0' || value[0] > maximum)
		return -EINVAL;
	mutex_lock(&cam_experiment_lock);
	WRITE_ONCE(*(unsigned int *)parameter->arg, value[0] - '0');
	mutex_unlock(&cam_experiment_lock);
	return 0;
}

static const struct kernel_param_ops cam_experiment_uint_ops = {
	.set = cam_experiment_uint_set,
	.get = param_get_uint,
};

static int cam_fps_set(const char *value, const struct kernel_param *parameter)
{
	size_t length = strlen(value);
	unsigned int mode;

	if (length && value[length - 1] == '\n')
		length--;
	if (length == 1 && (value[0] == '0' || value[0] == '1'))
		mode = value[0] - '0';
	else if (length == 2 && value[1] == '0' && (value[0] == '3' || value[0] == '6'))
		mode = (value[0] - '0') * 10;
	else
		return -EINVAL;
	mutex_lock(&cam_experiment_lock);
	WRITE_ONCE(*(unsigned int *)parameter->arg, mode);
	mutex_unlock(&cam_experiment_lock);
	return 0;
}

static const struct kernel_param_ops cam_fps_ops = {
	.set = cam_fps_set,
	.get = param_get_uint,
};
module_param_cb(fps, &cam_fps_ops, &cam_experiment_config.fps, 0644);
MODULE_PARM_DESC(fps, "Frame rate: 0 unchanged, 1 V4L2 selects 30/60 (default), 30 or 60 fixed");

unsigned int cam_fps_mode(void)
{
	return READ_ONCE(cam_experiment_config.fps);
}

module_param_cb(colour, &cam_experiment_uint_ops, &cam_experiment_config.colour, 0644);
MODULE_PARM_DESC(colour, "Colour: 0 off, 1 lens shading disabled (default)");
module_param_cb(framing, &cam_experiment_uint_ops, &cam_experiment_config.framing, 0644);
MODULE_PARM_DESC(framing, "Framing: 0 off, 1 log crop/zoom, 2 pin zoom to 1x (default)");

static_assert(CAM_EXTRA_SIZE > 36 * SZ_1M);

static bool cam_setfile_valid(const struct firmware *blob)
{
	const u8 *message;

	if (blob->size != CAM_SETFILE_SIZE || get_unaligned_be32(blob->data) != 0x22240106 ||
	    get_unaligned_le32(blob->data + 0x10) > 0x20)
		return false;
	message = blob->data + CAM_LENSCAL_MESSAGE;
	return get_unaligned_le32(blob->data + CAM_LENSCAL_RECORD) == CAM_LENSCAL_SIZE &&
		get_unaligned_le16(message + 4) == 0x50f &&
		!get_unaligned_le32(message + 8) &&
		get_unaligned_le32(message + 0x14) == 0x958;
}

void cam_experiment_begin(struct cam_device *cam)
{
	struct cam_experiment *e = &cam->session.experiment;
	const char *name = "apple/isp_2224_01XX.dat";
	int ret;

	lockdep_assert_held(&cam->fw_lock);
	mutex_lock(&cam_experiment_lock);
	e->config = cam_experiment_config;
	mutex_unlock(&cam_experiment_lock);
	e->frame_rate = e->config.fps == 1 ? (cam->frame_rate ?: 30) : e->config.fps;
	WRITE_ONCE(cam_setfile_result, 0);
	WRITE_ONCE(cam_lenscal_result, 0);
	e->colour_active = e->config.colour;
	if (e->colour_active)
		dev_info(cam->dev, "colour=1 latched: canonical setfile, lens shading disabled\n");
	if (!e->colour_active && !e->config.framing)
		return;
	dev_info(cam->dev, "experiment: setfile=%s position=%u lenscal=%u%s\n",
		 e->colour_active ? name : "none", e->colour_active ? 2 : 0,
		 e->colour_active ? 2 : 0, "");
	if (READ_ONCE(cam->fw_sessions) != 1 && !e->colour_active) {
		dev_info(cam->dev, "experiment skipped: not the first session of this firmware start\n");
		return;
	}
	if (READ_ONCE(cam->fw_sessions) == 1) {
		e->framing_enabled = e->config.framing;
		e->framing_pending = e->framing_enabled;
	}
	if (e->framing_enabled)
		dev_info(cam->dev, "framing=%u latched for session\n", e->config.framing);
	e->enabled = e->colour_active;
	if (!e->enabled)
		return;
	ret = request_firmware(&e->blob, name, cam->dev);
	if (!ret && cam_setfile_valid(e->blob))
		return;
	dev_info(cam->dev, "colour skipped: invalid or unavailable blob %s (%d)\n", name, ret);
	WRITE_ONCE(cam_setfile_result, -EINVAL);
	WRITE_ONCE(cam_lenscal_result, -EINVAL);
	e->enabled = false;
}

void cam_experiment_end(struct cam_device *cam)
{
	struct cam_experiment *e = &cam->session.experiment;

	if (e->blob)
		release_firmware(e->blob);
	e->blob = NULL;
	e->enabled = false;
	e->colour_active = false;
	e->framing_enabled = false;
	e->framing_pending = false;
}

/* A replied rejection disables the rest of this session's experiment. The
 * exchange still uses cam_message, so missing replies retain its DEAD latch.
 */
static int cam_experiment_continue(struct cam_device *cam, int ret)
{
	if (ret)
		cam->session.experiment.enabled = false;
	return ret == -EPROTO && !cam_dead(cam) ? 0 : ret;
}

int cam_experiment_round(struct cam_device *cam)
{
	struct cam_experiment *e = &cam->session.experiment;
	int ret = 0;

	if (!e->enabled)
		return 0;
	ret = cam_lenscal_set(cam, 1);
	if (!ret && !cam_dead(cam))
		ret = cam_lenscal_set(cam, 2);
	if (!ret && !cam_dead(cam))
		ret = cam_lenscal_get(cam);
	return cam_experiment_continue(cam, ret);
}

/* After CH_START, before framing, frame-rate pinning and capture buffers. */
int cam_experiment_load(struct cam_device *cam)
{
	struct cam_experiment *e = &cam->session.experiment;
	int ret;

	if (!e->enabled)
		return 0;
	ret = cam_lenscal_get(cam);
	if (!ret && !cam_dead(cam))
		ret = cam_setfile_stage(cam);
	if (!ret && !cam_dead(cam))
		ret = cam_setfile_load(cam);
	return cam_experiment_continue(cam, ret);
}

static int cam_framing_continue(struct cam_device *cam, int ret)
{
	struct cam_experiment *e = &cam->session.experiment;

	if (ret) {
		e->framing_enabled = false;
		e->framing_pending = false;
		dev_info(cam->dev, "framing disabled for session: result=%d\n", ret);
	}
	return ret == -EPROTO && !cam_dead(cam) ? 0 : ret;
}

/* S3.5: CH_START and any S3 setfile load have both replied. */
int cam_framing_start(struct cam_device *cam)
{
	struct cam_experiment *e = &cam->session.experiment;
	u16 zoom[2];
	int ret;

	lockdep_assert_held(&cam->fw_lock);
	if (!e->framing_enabled || cam_dead(cam))
		return 0;
	ret = cam_framing_crop(cam, "S3.5");
	if (!ret && !cam_dead(cam))
		ret = cam_framing_zoom(cam, "S3.5", zoom);
	if (!ret && !cam_dead(cam) && e->config.framing == 2) {
		ret = cam_framing_pin(cam);
		if (!ret && !cam_dead(cam)) {
			dev_info(cam->dev, "framing S3.5: pin zoom=0x400\n");
			ret = cam_framing_zoom(cam, "S3.5-pinned", zoom);
		}
	}
	return cam_framing_continue(cam, ret);
}

/* Stream thread, under fw_lock, after a successful requeue and any S5 load. */
int cam_framing_requeue(struct cam_device *cam)
{
	/* About one poll a second: only drift logs, at most ten lines a minute. */
	static DEFINE_RATELIMIT_STATE(drift, 60 * HZ, 10);
	struct cam_experiment *e = &cam->session.experiment;
	bool s5 = e->framing_pending;
	const char *when = s5 ? "S5" : "periodic";
	u16 zoom[2];
	int ret;

	lockdep_assert_held(&cam->fw_lock);
	if (!e->framing_enabled || cam_dead(cam))
		return 0;
	/* The setfile runs near 60 fps: poll every 60 delivered frames after S5. */
	if (!s5 && (e->config.framing != 2 ||
	    (u32)(cam->session.delivered - e->framing_delivered) < 60))
		return 0;
	e->framing_pending = false;
	e->framing_delivered = cam->session.delivered;
	ret = cam_framing_zoom(cam, s5 ? when : NULL, zoom);
	if (!ret && !cam_dead(cam) && e->config.framing == 2 &&
	    (zoom[0] != 0x400 || zoom[1] != 0x400)) {
		/* Logged before the SET, so a failed SET keeps the drift that caused it. */
		if (__ratelimit(&drift))
			dev_info(cam->dev, "framing %s: re-assert zoom=0x400 scaler0=%#x scaler1=%#x\n",
				 when, zoom[0], zoom[1]);
		ret = cam_framing_pin(cam);
	}
	return cam_framing_continue(cam, ret);
}
