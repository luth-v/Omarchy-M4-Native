# SPDX-License-Identifier: GPL-2.0-only
"""Reject 1080p regressions in disposable sources; all input data is synthetic."""
import os
from pathlib import Path
import resource
import shutil
import subprocess
import sys
import tempfile

DRIVER = Path(__file__).resolve().parents[1]
ENV = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1', 'UBSAN_OPTIONS': 'halt_on_error=1'}
FLAGS = ['gcc', '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
         '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', '-fsanitize=address,undefined']
WIRE, CMD, FW, EXP, V4L2, HEADER, MAIN = ('j713_cam_wire.h', 'j713_cam_cmd.c', 'j713_cam_fw.c',
                                        'j713_cam_experiment.c', 'j713_cam_v4l2.c', 'j713_cam.h',
                                        'j713_cam_main.c')
mutations = []


def mutation(name, file, old, new, harness='highres_test'):
    mutations.append((name, file, old, new, harness))


mutation('small capture reservation', WIRE, '#define CAM_CAPTURE_MAX 0xdc0000UL',
         '#define CAM_CAPTURE_MAX 0xd00000UL')
mutation('small identity mapping', HEADER, 'CAM_ARENA_SIZE + CAM_CAPTURE_MAX)',
         'CAM_ARENA_SIZE + CAM_CAPTURE_SIZE)')
mutation('720p pool enlarged', WIRE, 'height == 1888 ? CAM_CAPTURE_MAX : CAM_CAPTURE_SIZE',
         'CAM_CAPTURE_MAX')
mutation('wrong crop centre', WIRE, '.crop_y = (height - deliver) / 2,',
         '.crop_y = (height - deliver) / 2 + 2,')
mutation('wrong UV crop centre', WIRE, '.crop_uv = (height - deliver) / 4,',
         '.crop_uv = (height - deliver) / 4 + 1,')
mutation('wrong Y crop count', WIRE, 'g->crop_y * g->width, g->deliver_y);',
         'g->crop_y * g->width, CAM_DELIVER_Y);', 'wire_test')
mutation('UV copy uses unaligned plane size', WIRE,
         'source + g->y_span + g->crop_uv * g->width,',
         'source + g->y_size + g->crop_uv * g->width,', 'wire_test')
mutation('AWB still starts at 720p UV', WIRE, 'u8 *uv = frame + g->deliver_y;',
         'u8 *uv = frame + CAM_DELIVER_Y;', 'wire_test')
mutation('AWB samples only 720 rows', WIRE, 'row < g->deliver_height;',
         'row < CAM_DELIVER_HEIGHT;', 'wire_test')
mutation('AWB updates only 720p bytes', WIRE, 'i < g->deliver_y / 2;',
         'i < CAM_DELIVER_Y / 2;', 'wire_test')
mutation('preset 5 selected', CMD, '{ 0x107, 0x10, 0, 0 },', '{ 0x107, 0x10, 0, 5 },')
mutation('wrong output height', CMD, 'put_unaligned_le32(g->height, command + 0x10);',
         'put_unaligned_le32(1080, command + 0x10);')
mutation('wrong output ref height', CMD, 'put_unaligned_le32(g->height, command + 0x2c);',
         'put_unaligned_le32(1080, command + 0x2c);')
mutation('wrong pool UV stride', CMD, 'put_unaligned_le32(g->width, command + 0x34);\n\t\t}',
         'put_unaligned_le32(1280, command + 0x34);\n\t\t}')
mutation('old frame offsets', CMD, 'index * g->frame_stride;', 'index * 0x254000;')
mutation('crop query sent at 1080p', EXP,
         'cam->geometry.width == 1920 ? 0 : cam_framing_crop(cam, "S3.5")',
         'cam_framing_crop(cam, "S3.5")')
mutation('output status rejection latches DEAD', CMD,
         'cam_experiment_exchange(cam, 0x38, 0, &result)', '((void)&result, cam_io(cam, 0x38, 0))')
mutation('clamp print ignored', WIRE,
         'if (strstr(text, "MaxFESOutput") && strstr(text, "Replaced"))', 'if (false)')
mutation('wrong output size accepted', WIRE,
         'if (width != 1920 || height != 1888)\n\t\te->rejected = true;',
         'if (false)\n\t\te->rejected = true;')
mutation('actual size ignored', WIRE, 'if (!strncmp(line, ", actual [", 10) &&', 'if (false &&')
mutation('evidence from the unprinted format', WIRE, 'line = strstr(text, "] CH = 0x");',
         'line = strstr(text, "ch=");')
mutation('other channel accepted', WIRE, '!used || channel || bes)', '!used || bes)')
mutation('other output accepted', WIRE, '!used || channel || bes)', '!used || channel)')
mutation('lost ring records accepted', WIRE,
         'if (le16_to_cpu(record->dropped) && !e->accepted)', 'if (false)')
mutation('lost records reject an accepted stream', WIRE,
         'if (le16_to_cpu(record->dropped) && !e->accepted)', 'if (le16_to_cpu(record->dropped))')
mutation('accepted before the capture list', CMD, '\tif (final) {\n', '\tif (true) {\n')
mutation('gate never accepts', CMD, '\t\te->accepted = true;\n', '')
mutation('initial positive evidence optional', CMD, 'if (ret || !e->seen || e->rejected)',
         'if (ret || e->rejected)')
mutation('gate rejects into DEAD', FW, 'if (!cam->session.output_rejected)', 'if (true)')
mutation('post-capture gate omitted', FW,
         '\t\tret = cam_capture(cam);\n\tif (!ret && !cam_dead(cam))\n\t\tret = cam_output_verify(cam, true);',
         '\t\tret = cam_capture(cam);')
mutation('larger pool not cleared', FW,
         'memset(cam_pointer(cam, cam->capture_base, cam->geometry.capture_size), 0,\n'
         '\t\t       cam->geometry.capture_size);',
         'memset(cam_pointer(cam, cam->capture_base, CAM_CAPTURE_SIZE), 0, CAM_CAPTURE_SIZE);')
mutation('late geometry mismatch copied', V4L2, 'if (session->output.rejected)',
         'if (false)', 'pacing_test')
mutation('format not latched at STREAMON', V4L2, '\tcam->geometry = cam->format;',
         '\t(void)cam->format;', 'lifecycle_test')
mutation('rejected session does not power-stop', V4L2,
         'if (cam->session.output_rejected && !cam_dead(cam))', 'if (false)', 'lifecycle_test')
mutation('format busy guard omitted', V4L2, 'if (vb2_is_busy(&cam->queue))', 'if (false)')
mutation('S_FMT changes active geometry', V4L2, 'cam_geometry_init(&cam->format,',
         'cam_geometry_init(&cam->geometry,')
mutation('second frame size missing', V4L2, 'CAM_CAPTURE_MAX ? 1U : 0U) ||', 'CAM_CAPTURE_MAX ? 0U : 0U) ||')
mutation('1080p size offered without its claim', V4L2, 'CAM_CAPTURE_MAX ? 1U : 0U) ||', '1U) ||')
mutation('1080p format accepted without its claim', V4L2,
         'bool hd = cam->capture_span == CAM_CAPTURE_MAX && pix->width == 1920',
         'bool hd = pix->width == 1920')
mutation('1080p interval listed without its claim', V4L2,
         '\t\t\t\t    cam->capture_span == CAM_CAPTURE_MAX))', '\t\t\t\t    true))')
mutation('STREAMON layout guard omitted', V4L2,
         '\t    cam->format.capture_size > cam->capture_span)', '\t    false)', 'lifecycle_test')
mutation('mid-capture rejection leaves the queue waiting', V4L2,
         '\t\tvb2_queue_error(&cam->queue);\n', '', 'pacing_test')
mutation('1080p lists 60 fps', V4L2,
         '\tif (interval->width == 1920 && interval->height == 1080)\n\t\tmode = 30;\n', '')
mutation('720p intervals clamped to 30', V4L2,
         'if (interval->width == 1920 && interval->height == 1080)', 'if (true)')
mutation('1080p G_PARM reports 60', V4L2, '\tif (cam->format.width == 1920)\n\t\trate = 30;\n', '')
mutation('720p G_PARM clamped to 30', V4L2, 'if (cam->format.width == 1920)\n\t\trate = 30;',
         'if (true)\n\t\trate = 30;')
mutation('1080p session pins 60', EXP,
         'if (e->frame_rate == 60 && cam->geometry.width == 1920) {', 'if (false) {')
mutation('720p session pin clamped to 30', EXP,
         'if (e->frame_rate == 60 && cam->geometry.width == 1920) {', 'if (e->frame_rate == 60) {',
         'fps_test')
mutation('1080p clamp drops the pin', EXP, '\t\te->frame_rate = 30;\n', '\t\te->frame_rate = 0;\n')
mutation('1080p clamp not logged', EXP,
         '\t\tdev_info(cam->dev, "fps: 60 not offered at 1080p, using 30\\n");\n', '')
mutation('1080p claim never tried', MAIN, '\t\tif (pfn + large <= end &&\n', '\t\tif (false &&\n',
         'claim_test')
mutation('1080p claim may leave the proven bounds', MAIN, '\t\tif (pfn + large <= end &&\n',
         '\t\tif (\n', 'claim_test')
mutation('original claim not retried at the same start', MAIN,
         '\t\tif (!alloc_contig_range(pfn, pfn + count, ACR_FLAGS_NONE, GFP_KERNEL))\n\t\t\tbreak;\n',
         '', 'claim_test')
mutation('walk bounded by the 1080p size', MAIN, 'last = end - count;', 'last = end - large;',
         'claim_test')
mutation('1080p span claimed for the original size', MAIN,
         'count == large ? CAM_CAPTURE_MAX : CAM_CAPTURE_SIZE', 'CAM_CAPTURE_MAX', 'claim_test')
mutation('claim tail not zeroed', MAIN, 'memset(cam->identity + PAGE_SIZE, 0, CAM_IDENTITY_SPAN(cam));',
         'memset(cam->identity + PAGE_SIZE, 0, CAM_IDENTITY_SIZE);', 'claim_test')
mutation('host window stays 720p', FW, 'CAM_IDENTITY_SPAN(cam), size, &offset))',
         'CAM_IDENTITY_SIZE, size, &offset))', 'claim_test')
mutation('host window always 1080p', FW, 'CAM_IDENTITY_SPAN(cam), size, &offset))',
         'CAM_IDENTITY_MAX, size, &offset))', 'claim_test')
mutation('1080p stride still 1280', V4L2, '.bytesperline = width,', '.bytesperline = 1280,')
mutation('queue still allocates 720p', V4L2, 'sizes[0] = cam->format.deliver_size;',
         'sizes[0] = CAM_DELIVER_SIZE;')
mutation('prepare still reports 720p', V4L2,
         'vb2_set_plane_payload(buffer, 0, cam->format.deliver_size);',
         'vb2_set_plane_payload(buffer, 0, CAM_DELIVER_SIZE);')
mutation('720p routed through the 1080p gate', CMD, 'if (cam->geometry.width != 1920)',
         'if (false)', 'sequence_test')
mutation('720p evidence watcher armed', CMD,
         '\tif (cam->geometry.width != 1920)\n\t\treturn cam_io(cam, 0x38, 0);',
         '\tif (cam->geometry.width != 1920) {\n\t\te->watching = true;\n'
         '\t\treturn cam_io(cam, 0x38, 0);\n\t}', 'sequence_test')


def run(command, root):
    return subprocess.run(command, cwd=root, env=ENV, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, timeout=90)


resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
if len(sys.argv) != 2:
    raise SystemExit('usage: mutate_highres.py SCRATCH_DIRECTORY')
scratch = Path(sys.argv[1]).resolve()
if not scratch.is_dir():
    raise SystemExit('Pass an existing scratch directory')
with tempfile.TemporaryDirectory(prefix='highres-mutations-', dir=scratch) as temporary:
    root = Path(temporary) / 'driver'
    (root / 'tests/.build').mkdir(parents=True)
    for pattern in ('j713*.c', 'j713*.h', 'tests/*.c', 'tests/*.h', 'tests/*.py', 'tests/*.json'):
        for source in DRIVER.glob(pattern):
            shutil.copy2(source, root / source.relative_to(DRIVER))
    originals = {file: (root / file).read_text() for _, file, _, _, _ in mutations}
    # Healthy binaries must pass before any mutant can count as rejected.
    for script in ('extract_session.py', 'extract_restart.py'):
        result = run(['python3', '-I', 'tests/' + script], root)
        if result.returncode:
            raise RuntimeError(result.stdout)
    for harness in ('highres_test', 'wire_test', 'pacing_test', 'lifecycle_test', 'claim_test',
                    'fps_test', 'sequence_test'):
        result = run([*FLAGS, f'tests/{harness}.c', '-o', f'tests/.build/{harness}'], root)
        if result.returncode:
            raise RuntimeError(result.stdout)
        result = run([f'tests/.build/{harness}'], root)
        if result.returncode:
            raise RuntimeError(result.stdout)
    result = run(['python3', '-I', 'tests/test_sequence.py'], root)
    if result.returncode:
        raise RuntimeError(result.stdout)
    for index, (name, file, old, new, harness) in enumerate(mutations, 1):
        source = originals[file]
        if source.count(old) != 1:
            raise RuntimeError(f'{name}: mutation anchor occurs {source.count(old)} times')
        (root / file).write_text(source.replace(old, new, 1))
        script = 'extract_restart.py' if harness == 'lifecycle_test' else 'extract_session.py'
        result = run(['python3', '-I', 'tests/' + script], root)
        phase = 'extraction'
        if not result.returncode:
            result = run([*FLAGS, f'tests/{harness}.c', '-o', f'tests/.build/{harness}'], root)
            phase = 'compile'
        if not result.returncode:
            if harness == 'sequence_test':
                result = run(['python3', '-I', 'tests/test_sequence.py'], root)
            else:
                result = run([f'tests/.build/{harness}'], root)
            phase = 'runtime'
        (root / file).write_text(source)
        if result.returncode == 0:
            raise RuntimeError(f'SURVIVED {index}: {name}')
        lines = [line for line in result.stdout.splitlines()
                 if 'Assertion' in line or 'Error' in line or 'error:' in line or 'FAILED' in line]
        detail = lines[0] if lines else result.stdout.splitlines()[-1:]
        print(f'REJECTED {index:02}: {name} ({phase}, exit {result.returncode}): {detail}', flush=True)
print(f'All {len(mutations)} 1080p regressions rejected; disposable sources removed.')
