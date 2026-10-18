// SPDX-License-Identifier: GPL-2.0-only
#include <media/v4l2-event.h>

#include "j713_cam.h"

static void cam_format(struct v4l2_pix_format *pix)
{
	*pix = (struct v4l2_pix_format) {
		.width = CAM_FRAME_WIDTH,
		.height = CAM_DELIVER_HEIGHT,
		.pixelformat = V4L2_PIX_FMT_NV12,
		.field = V4L2_FIELD_NONE,
		.bytesperline = CAM_FRAME_WIDTH,
		.sizeimage = CAM_DELIVER_SIZE,
		.colorspace = V4L2_COLORSPACE_REC709,
		.xfer_func = V4L2_XFER_FUNC_709,
		.ycbcr_enc = V4L2_YCBCR_ENC_709,
		.quantization = V4L2_QUANTIZATION_LIM_RANGE,
	};
}

static int cam_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, "j713-isp-cam", sizeof(cap->driver));
	strscpy(cap->card, "FaceTime HD Camera (J713)", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:j713-isp-cam", sizeof(cap->bus_info));
	return 0;
}

static int cam_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *fmt)
{
	if (fmt->index)
		return -EINVAL;
	fmt->pixelformat = V4L2_PIX_FMT_NV12;
	return 0;
}

static int cam_get_fmt(struct file *file, void *priv, struct v4l2_format *fmt)
{
	cam_format(&fmt->fmt.pix);
	return 0;
}

static int cam_set_fmt(struct file *file, void *priv, struct v4l2_format *fmt)
{
	struct cam_device *cam = video_drvdata(file);

	if (vb2_is_busy(&cam->queue))
		return -EBUSY;
	return cam_get_fmt(file, priv, fmt);
}

static int cam_enum_sizes(struct file *file, void *priv, struct v4l2_frmsizeenum *size)
{
	if (size->index || size->pixel_format != V4L2_PIX_FMT_NV12)
		return -EINVAL;
	size->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	size->discrete.width = CAM_FRAME_WIDTH;
	size->discrete.height = CAM_DELIVER_HEIGHT;
	return 0;
}

static int cam_enum_intervals(struct file *file, void *priv, struct v4l2_frmivalenum *interval)
{
	struct cam_device *cam = video_drvdata(file);
	unsigned int mode = vb2_is_streaming(&cam->queue) ?
		cam->session.experiment.config.fps : cam_fps_mode();

	if (interval->index > (mode == 1 ? 1 : 0) ||
	    interval->pixel_format != V4L2_PIX_FMT_NV12 ||
	    interval->width != CAM_FRAME_WIDTH || interval->height != CAM_DELIVER_HEIGHT)
		return -EINVAL;
	interval->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	interval->discrete = (struct v4l2_fract) {
		1, mode == 60 || (mode == 1 && interval->index) ? 60 : 30 };
	return 0;
}

static int cam_parm(struct file *file, void *priv, struct v4l2_streamparm *parm)
{
	struct cam_device *cam = video_drvdata(file);
	unsigned int mode = cam_fps_mode();
	unsigned int rate;

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	if (vb2_is_streaming(&cam->queue))
		rate = cam->session.experiment.frame_rate ?: 30;
	else
		rate = mode == 1 ? (cam->frame_rate ?: 30) : (mode ?: 30);
	memset(&parm->parm.capture, 0, sizeof(parm->parm.capture));
	parm->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	parm->parm.capture.timeperframe = (struct v4l2_fract) { 1, rate };
	return 0;
}

static int cam_set_parm(struct file *file, void *priv, struct v4l2_streamparm *parm)
{
	struct cam_device *cam = video_drvdata(file);
	struct v4l2_fract *time = &parm->parm.capture.timeperframe;
	bool streaming = vb2_is_streaming(&cam->queue);
	unsigned int mode = streaming ? cam->session.experiment.config.fps : cam_fps_mode();

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	/* Fixed modes, like fps=0, report their only interval at any time. */
	if (mode == 1 && streaming)
		return -EBUSY;
	if (mode == 1) {
		/* Nearest interval; a zero fraction or a tie selects 1/30. */
		cam->frame_rate = time->numerator && time->denominator &&
			(u64)time->numerator * 40 < time->denominator ? 60 : 30;
	}
	return cam_parm(file, priv, parm);
}

static int cam_enum_input(struct file *file, void *priv, struct v4l2_input *input)
{
	if (input->index)
		return -EINVAL;
	*input = (struct v4l2_input) { .type = V4L2_INPUT_TYPE_CAMERA };
	strscpy(input->name, "Camera", sizeof(input->name));
	return 0;
}

static int cam_get_input(struct file *file, void *priv, unsigned int *input)
{
	*input = 0;
	return 0;
}

static int cam_set_input(struct file *file, void *priv, unsigned int input)
{
	return input ? -EINVAL : 0;
}

static int cam_queue_setup(struct vb2_queue *queue, unsigned int *buffers,
			   unsigned int *planes, unsigned int sizes[], struct device *devices[])
{
	if (*planes)
		return *planes != 1 || sizes[0] < CAM_DELIVER_SIZE ? -EINVAL : 0;
	*planes = 1;
	sizes[0] = CAM_DELIVER_SIZE;
	return 0;
}

static int cam_buffer_init(struct vb2_buffer *buffer)
{
	struct cam_buffer_vb2 *buf = container_of(to_vb2_v4l2_buffer(buffer),
						struct cam_buffer_vb2, vb);

	INIT_LIST_HEAD(&buf->list);
	return 0;
}

static int cam_buffer_prepare(struct vb2_buffer *buffer)
{
	if (vb2_plane_size(buffer, 0) < CAM_DELIVER_SIZE || !vb2_plane_vaddr(buffer, 0))
		return -EINVAL;
	vb2_set_plane_payload(buffer, 0, CAM_DELIVER_SIZE);
	return 0;
}

static void cam_buffer_queue(struct vb2_buffer *buffer)
{
	struct cam_device *cam = vb2_get_drv_priv(buffer->vb2_queue);
	struct cam_buffer_vb2 *buf = container_of(to_vb2_v4l2_buffer(buffer),
						struct cam_buffer_vb2, vb);
	unsigned long flags;

	spin_lock_irqsave(&cam->buffers_lock, flags);
	list_add_tail(&buf->list, &cam->buffers);
	spin_unlock_irqrestore(&cam->buffers_lock, flags);
}

void cam_return_buffers(struct cam_device *cam, enum vb2_buffer_state state)
{
	struct cam_buffer_vb2 *buffer, *next;
	unsigned long flags;
	LIST_HEAD(done);

	spin_lock_irqsave(&cam->buffers_lock, flags);
	list_splice_init(&cam->buffers, &done);
	spin_unlock_irqrestore(&cam->buffers_lock, flags);
	list_for_each_entry_safe(buffer, next, &done, list) {
		list_del_init(&buffer->list);
		vb2_buffer_done(&buffer->vb.vb2_buf, state);
	}
}

void cam_deliver(struct cam_device *cam, unsigned int slot, u64 timestamp)
{
	struct cam_session *session = &cam->session;
	struct cam_buffer_vb2 *buffer = NULL;
	const u8 *source;
	unsigned long flags;
	u32 sequence = session->sequence++;
	u64 start;
	u8 *dest;

	spin_lock_irqsave(&cam->buffers_lock, flags);
	if (!list_empty(&cam->buffers)) {
		buffer = list_first_entry(&cam->buffers, struct cam_buffer_vb2, list);
		list_del_init(&buffer->list);
	}
	spin_unlock_irqrestore(&cam->buffers_lock, flags);
	if (!buffer) {
		session->dropped++;
		return;
	}
	source = cam_pointer(cam, cam->capture_base + CAM_RENDER_OFFSET +
			      slot * cam->geometry.frame_stride, cam->geometry.frame_stride);
	dest = vb2_plane_vaddr(&buffer->vb.vb2_buf, 0);
	if (!source || !dest) {
		session->dropped++;
		vb2_buffer_done(&buffer->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		cam_fail(cam, "frame copy range unavailable");
		return;
	}
	start = ktime_get_ns();
	dma_rmb();
	cam_crop_copy(&cam->geometry, dest, source);
	if (READ_ONCE(cam->auto_white_balance))
		cam_white_balance(&session->wb, dest);
	session->copy_ns += ktime_get_ns() - start;
	session->delivered++;
	buffer->vb.vb2_buf.timestamp = timestamp;
	buffer->vb.sequence = sequence;
	buffer->vb.field = V4L2_FIELD_NONE;
	vb2_set_plane_payload(&buffer->vb.vb2_buf, 0, CAM_DELIVER_SIZE);
	vb2_buffer_done(&buffer->vb.vb2_buf, VB2_BUF_STATE_DONE);
	start = ktime_get_ns();
	if (session->delivered > 1) {
		u64 gap = start - session->last_delivery_ns;

		session->pacing_gaps++;
		session->pacing_gap_ns += gap;
		session->pacing_max_gap_ns = max(session->pacing_max_gap_ns, gap);
	}
	session->last_delivery_ns = start;
	dev_dbg(cam->dev, "frame=%u slot=%u timestamp=%llu\n", sequence, slot, timestamp);
}

static int cam_start_streaming(struct vb2_queue *queue, unsigned int count)
{
	struct cam_device *cam = vb2_get_drv_priv(queue);
	int ret = -EIO;

	if (cam_dead(cam) || READ_ONCE(cam->shutting_down))
		goto failed;
	cancel_delayed_work_sync(&cam->idle_work);
	cam_set_state(cam, CAM_STREAMING);
	if (!cam->firmware_running) {
		ret = cam_lifecycle(cam, cam_boot);
		if (ret)
			goto failed;
	}
	ret = cam_lifecycle(cam, cam_session_start);
	if (ret)
		goto failed;
	/* STREAMOFF must not stop the task before kthread invokes threadfn. */
	wait_for_completion(&cam->stream_started);
	schedule_delayed_work(&cam->idle_work, HZ);
	return 0;
failed:
	cam_return_buffers(cam, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void cam_stop_streaming(struct vb2_queue *queue)
{
	struct cam_device *cam = vb2_get_drv_priv(queue);

	cancel_delayed_work_sync(&cam->idle_work);
	cam_stream_join(cam);
	/* The firmware hangs on its third CH_START: one session per start.
	 * The result is not needed: any failure has already latched DEAD.
	 */
	if (cam_fw_restart)
		cam_lifecycle(cam, cam_fw_stop);
	cam_return_buffers(cam, VB2_BUF_STATE_ERROR);
	cam_set_state(cam, CAM_IDLE);
	if (!cam_dead(cam) && !READ_ONCE(cam->shutting_down) && cam->firmware_running)
		schedule_delayed_work(&cam->idle_work, HZ);
}

static int cam_control_set(struct v4l2_ctrl *control)
{
	struct cam_device *cam = container_of(control->handler, struct cam_device, controls);

	if (control->id != V4L2_CID_AUTO_WHITE_BALANCE)
		return -EINVAL;
	WRITE_ONCE(cam->auto_white_balance, !!control->val);
	return 0;
}

static const struct vb2_ops cam_queue_ops = {
	.queue_setup = cam_queue_setup,
	.buf_init = cam_buffer_init,
	.buf_prepare = cam_buffer_prepare,
	.buf_queue = cam_buffer_queue,
	.start_streaming = cam_start_streaming,
	.stop_streaming = cam_stop_streaming,
};

static const struct v4l2_ctrl_ops cam_control_ops = { .s_ctrl = cam_control_set };

static const struct v4l2_file_operations cam_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

static const struct v4l2_ioctl_ops cam_ioctl_ops = {
	.vidioc_querycap = cam_querycap,
	.vidioc_enum_fmt_vid_cap = cam_enum_fmt,
	.vidioc_g_fmt_vid_cap = cam_get_fmt,
	.vidioc_try_fmt_vid_cap = cam_get_fmt,
	.vidioc_s_fmt_vid_cap = cam_set_fmt,
	.vidioc_enum_framesizes = cam_enum_sizes,
	.vidioc_enum_frameintervals = cam_enum_intervals,
	.vidioc_g_parm = cam_parm,
	.vidioc_s_parm = cam_set_parm,
	.vidioc_enum_input = cam_enum_input,
	.vidioc_g_input = cam_get_input,
	.vidioc_s_input = cam_set_input,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
	.vidioc_log_status = v4l2_ctrl_log_status,
};

static void cam_v4l2_release(struct v4l2_device *v4l2)
{
	struct cam_device *cam = container_of(v4l2, struct cam_device, v4l2);

	if (!cam_memory_release(cam))
		return;
	if (cam->queue_ready)
		vb2_queue_release(&cam->queue);
	v4l2_ctrl_handler_free(&cam->controls);
	kvfree(cam->arena.entries);
	put_device(cam->dev);
	kfree(cam);
}

int cam_v4l2_register(struct cam_device *cam)
{
	struct vb2_queue *queue = &cam->queue;
	struct video_device *video = &cam->video;
	int ret;

	/* The V4L2 reference owns private state even on a registration failure. */
	cam->v4l2.release = cam_v4l2_release;
	ret = v4l2_device_register(cam->dev, &cam->v4l2);
	if (ret) {
		cam_v4l2_release(&cam->v4l2);
		return ret;
	}
	v4l2_ctrl_handler_init(&cam->controls, 1);
	v4l2_ctrl_new_std(&cam->controls, &cam_control_ops, V4L2_CID_AUTO_WHITE_BALANCE, 0, 1, 1, 1);
	ret = cam->controls.error;
	if (ret)
		goto unregister;
	cam->v4l2.ctrl_handler = &cam->controls;
	queue->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	queue->io_modes = VB2_MMAP | VB2_DMABUF;
	queue->drv_priv = cam;
	queue->buf_struct_size = sizeof(struct cam_buffer_vb2);
	queue->ops = &cam_queue_ops;
	queue->mem_ops = &vb2_vmalloc_memops;
	queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC | V4L2_BUF_FLAG_TSTAMP_SRC_EOF;
	queue->lock = &cam->video_lock;
	queue->dev = cam->dev;
	queue->min_queued_buffers = 0;
	ret = vb2_queue_init(queue);
	if (ret)
		goto unregister;
	WRITE_ONCE(cam->queue_ready, true);
	strscpy(video->name, "FaceTime HD Camera (J713)", sizeof(video->name));
	video->v4l2_dev = &cam->v4l2;
	video->fops = &cam_fops;
	video->ioctl_ops = &cam_ioctl_ops;
	video->release = video_device_release_empty;
	video->lock = &cam->video_lock;
	video->queue = queue;
	video->vfl_dir = VFL_DIR_RX;
	video->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	video_set_drvdata(video, cam);
	ret = video_register_device(video, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto unregister;
	return 0;
unregister:
	v4l2_device_unregister(&cam->v4l2);
	v4l2_device_put(&cam->v4l2);
	return ret;
}

void cam_v4l2_unregister(struct cam_device *cam)
{
	video_unregister_device(&cam->video);
	v4l2_device_unregister(&cam->v4l2);
	v4l2_device_put(&cam->v4l2);
}
