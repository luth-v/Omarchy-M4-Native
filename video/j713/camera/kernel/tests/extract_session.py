# SPDX-License-Identifier: GPL-2.0-only
"""Compile the production session, transport and V4L2 callbacks with host stubs."""
from pathlib import Path
import re

KERNEL = Path(__file__).resolve().parents[1]
OUT = KERNEL / 'tests/.build'
OUT.mkdir(exist_ok=True)


def function(source, name):
    match = re.search(r'^(?:static )?(?:int|void|bool|unsigned int) \*?' + name +
                      r'\([^;]+?\n\{\n.*?^}\n', source, re.M | re.S)
    if match is None:
        raise RuntimeError('Cannot extract ' + name)
    return match[0]


header = (KERNEL / 'j713_cam.h').read_text()
config = (KERNEL / 'j713_cam_experiment.c').read_text()
command = (KERNEL / 'j713_cam_cmd.c').read_text()
firmware = (KERNEL / 'j713_cam_fw.c').read_text()
v4l2 = (KERNEL / 'j713_cam_v4l2.c').read_text()
log = (KERNEL / 'j713_cam_log.c').read_text()
types = [re.search(r'^struct ' + name + r' \{\n.*?^};', header, re.M | re.S)[0]
         for name in ('cam_experiment_config', 'cam_experiment', 'cam_session')]
(OUT / 'experiment_types.h').write_text('#define CAM_HAS_FRAMING 1\n#define CAM_HAS_FPS 1\n' + '\n'.join(types))
output = [function(command, 'cam_setfile_stage')]
output.extend(function(config, name) for name in (
    'cam_experiment_uint_set', 'cam_setfile_valid', 'cam_experiment_begin', 'cam_experiment_end',
    'cam_experiment_continue', 'cam_experiment_round', 'cam_experiment_load',
    'cam_framing_continue', 'cam_framing_start', 'cam_framing_requeue'))
command = command.replace(function(command, 'cam_setfile_stage'), '')
output.append('\n'.join(line for line in command.splitlines() if not line.startswith('#include')))
output.extend(function(firmware, name) for name in (
    'cam_command_name', 'cam_message', 'cam_exchange', 'cam_session_stop',
    'cam_batch', 'cam_stream_thread', 'cam_session_setup', 'cam_session_start'))
(OUT / 'experiment_impl.h').write_text('\n'.join(output))
(OUT / 'fps_params_impl.h').write_text('\n'.join(function(config, name) for name in ('cam_fps_set', 'cam_fps_mode')))
(OUT / 'fps_v4l2_impl.h').write_text('\n'.join(function(v4l2, name) for name in
                                              ('cam_enum_intervals', 'cam_parm', 'cam_set_parm')))
(OUT / 'print_impl.h').write_text('\n'.join((function(firmware, 'cam_print_other'),
                                          function(firmware, 'cam_print_poll'),
                                          function(log, 'cam_dead_print_dump'))))
defaults = re.search(r'^static struct cam_experiment_config cam_experiment_config = \{\n(.*?)\n};', config, re.M | re.S)
if sorted(re.findall(r'\.([a-z_]+) = ([0-9]+),', defaults[1])) != [('colour', '1'), ('fps', '1'), ('framing', '2')]:
    raise RuntimeError('Production defaults changed')
(OUT / 'experiment_defaults.h').write_text(defaults[0].replace('cam_experiment_config =', 'cam_production_defaults ='))
(OUT / 'pacing_impl.h').write_text('\n'.join((function(v4l2, 'cam_deliver'),
                                           function(firmware, 'cam_batch'),
                                           function(firmware, 'cam_stream_thread'))))

(OUT / 'snapshot_impl.h').write_text(function((KERNEL / 'j713_isp_d_dt_loader.c').read_text(), 'd_snapshot_firmware'))
