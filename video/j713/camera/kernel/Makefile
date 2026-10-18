# SPDX-License-Identifier: GPL-2.0-only
obj-m += j713_isp_d_dt.o j713_isp_cam.o
j713_isp_d_dt-y := j713_isp_d_dt_loader.o j713-isp-stage-d.dtbo.o
j713_isp_cam-y := j713_cam_main.o j713_cam_fw.o j713_cam_cmd.o j713_cam_v4l2.o \
	j713_cam_experiment.o j713_cam_log.o
DTC_FLAGS_j713-isp-stage-d := -Wno-interrupts_property
