#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Luthfi Hizb
"""Load the J713 camera at boot (m4-camera).

m4-j713-camera.service runs this with --boot after udev has triggered and the
speaker registration has finished. It repeats the manual runner's load
(run-on-air.sh load) with the module's built-in defaults: the per-boot Stage D
loader j713_isp_d_dt (overlay, DARTs, mailbox), the media modules, a sync,
memory compaction, then j713_isp_cam. The probe holds about 599 MiB until
power-off and registers "FaceTime HD Camera (J713)". The firmware does not
start here; it starts at the first STREAMON. Only the installed files, only
when they match their SHA256SUMS, only on the J713 and only on the kernel the
modules were built for.

One attempt per boot: like the runner, it creates the Stage D attempt
directory boot-<boot_id>-1 right before the camera module loads, and refuses
once any attempt for this boot exists. A failed load leaves the camera
unavailable until a physical power-off; the rest of the system keeps running.
Nothing retries and nothing unloads.

One strike: an attempt marker is written durably before the first module
loads and removed once the load returns, success or failure. If a boot dies
while loading, the next boot loads nothing, because the Air has no boot menu
to recover from. --start loads in the running session instead (and clears
such a marker). Inspection is the default.

Off switch: --disable writes a hold file, and no boot or --start loads the
camera until --enable removes it. A loaded camera stays until power-off.
"""
import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import sys
import tempfile
import time

BOARD = b'apple,j713'
COMPATIBLE = 'sys/firmware/devicetree/base/compatible'
OSRELEASE = 'proc/sys/kernel/osrelease'
BOOT_ID = 'proc/sys/kernel/random/boot_id'
COMPACT = 'proc/sys/vm/compact_memory'
INSTALLED = 'usr/lib/omarchy-m4/j713-camera'
SUMS = 'SHA256SUMS'
RELEASE = '7.1.9-m4-gpu-backport-20261008'
TUNING = 'usr/lib/firmware/apple/isp_2224_01XX.dat'
TUNING_SHA256 = '1e25d89b7acc3b62fcb64c5102503ba289c3cb000ca6abe812c3249ac6349f77'
QUALIFIED_FIRMWARE = (0xaaafbaa6,)
PINS = {
    'j713_isp_d_dt.ko': 'c6d115ec5ab1f465a2f6c088ec431bb19e5cf437b6247f1cd95032b1b17c634a',
    'j713_isp_cam.ko': 'ddcdf32476245d0cb8d803bedcc48d721b7fc1a3677e86ccea86abc3177d01a8',
    'health-on-air.py': 'fa3c3b9e201df1e7fc6dda2789087114695705019895505282ea2f37285042d1',
}
STATE = 'var/lib/omarchy-m4/camera'
# Shared with run-on-air.sh and Stage D: the lock and the per-boot attempt directories.
RUNNER_STATE = 'var/tmp/j713-isp-stageD-state'
WANTS = 'etc/systemd/system/multi-user.target.wants/m4-j713-camera.service'
# Load order: the camera binds to the DARTs and mailbox the loader adds.
LOADER = ('j713_isp_d_dt', 'j713_isp_d_dt.ko')
CAMERA = ('j713_isp_cam', 'j713_isp_cam.ko')
MODULES = (LOADER, CAMERA)
HEALTH = 'health-on-air.py'
FILES = (*(name for _, name in MODULES), HEALTH)
DIAGNOSTIC = 'j713_isp_d'
STAGE_C = ('j713_isp_c', 'j713_isp_c_dt')
# insmod does not resolve the camera's dependencies.
MEDIA = ('videodev', 'videobuf2-v4l2', 'videobuf2-vmalloc')
DARTS = ('48e8c0000', '48e8e0000', '48e900000')
MAILBOX = '48d608000.mailbox'
NAME = 'FaceTime HD Camera (J713)'
PARAMETERS = ('state', 'dead_reason', 'fw_starts', 'sessions', 'colour', 'framing', 'fps')
# States after a successful probe; dead and unbound are not.
HEALTHY = ('probed', 'idle', 'streaming')
# Each provider gets 5 s to bind, as in the runner.
WAIT_STEPS, WAIT_STEP = 50, 0.1
SPENT = 'The camera is unavailable until a physical power-off. The rest of the system is fine.'


class Refused(Exception):
    """A guard stopped the load before anything was loaded."""


class Failed(Exception):
    """The load ran and failed."""


def require(condition, message, error=Refused):
    if not condition:
        raise error(message)


def read(path):
    try:
        return path.read_text().strip()
    except (OSError, UnicodeDecodeError):
        return None


def sync_directory(path):
    directory = os.open(path, os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def log(message):
    # Progress goes to the journal as it happens, so a boot that dies shows the last step.
    print('m4-camera: ' + message, file=sys.stderr, flush=True)


def module_release(data):
    """The kernel release in an ELF64 module's .modinfo vermagic, or None."""
    try:
        if data[:6] != b'\x7fELF\x02\x01':
            return None
        (shoff,) = struct.unpack_from('<Q', data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3a)
        if shentsize != 64 or not 0 < shstrndx < shnum:
            return None
        # name, type, flags, address, offset, size
        headers = [struct.unpack_from('<IIQQQQ', data, shoff + index * 64) for index in range(shnum)]

        def content(header):
            if header[1] == 8 or header[4] + header[5] > len(data):  # SHT_NOBITS or truncated
                raise ValueError('section outside the file')
            return data[header[4]:header[4] + header[5]]

        names = content(headers[shstrndx])
        found = [header for header in headers if names[header[0]:].split(b'\0', 1)[0] == b'.modinfo']
        if len(found) != 1:
            return None
        values = [entry[len(b'vermagic='):] for entry in content(found[0]).split(b'\0')
                  if entry.startswith(b'vermagic=')]
        if len(values) != 1:
            return None
        return values[0].split(b' ')[0].decode('ascii') or None
    except (struct.error, ValueError, UnicodeDecodeError):
        return None


def root_file(path, lstat):
    """None for a regular, root-owned, non-symlink file, else why not."""
    try:
        info = lstat(path)
    except FileNotFoundError:
        return 'missing'
    if stat.S_ISLNK(info.st_mode):
        return 'a symlink'
    if not stat.S_ISREG(info.st_mode):
        return 'not a regular file'
    if info.st_uid != 0:
        return 'not owned by root'
    if info.st_mode & 0o022:
        return 'writable by group or others'
    return None


def directory_problem(root, relative, lstat, create=False):
    """Check every parent before using it; only root may change these paths."""
    path = root
    for part in Path(relative).parts:
        path = path / part
        try:
            info = lstat(path)
        except FileNotFoundError:
            if not create:
                return 'missing'
            path.mkdir(mode=0o755)
            sync_directory(path.parent)
            info = lstat(path)
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0:
            return 'directory is a symlink or not owned by root'
        if info.st_mode & 0o022:
            return 'directory is writable by group or others'
    return None


def read_root_file(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, 'rb') as stream:
        return stream.read()


def parse_sums(data):
    """The hashes, or None unless SHA256SUMS lists both modules and the health script."""
    try:
        text = data.decode('ascii')
    except UnicodeDecodeError:
        return None
    if not text.endswith('\n'):
        return None
    sums = {}
    for line in text[:-1].split('\n'):
        match = re.fullmatch(r'([0-9a-f]{64})  (\S+)', line)
        if not match or match[2] in sums:
            return None
        sums[match[2]] = match[1]
    return sums if set(sums) == set(FILES) else None


def verify(root, lstat):
    """Check the installed files: ({file: 'ok' or why not}, {module file: bytes})."""
    directory = root / INSTALLED
    bad = directory_problem(root, INSTALLED, lstat)
    files, data = {}, {}
    for filename in (SUMS, *FILES):
        problem = bad or root_file(directory / filename, lstat)
        if problem is None:
            try:
                data[filename] = read_root_file(directory / filename)
            except OSError as error:
                problem = 'unreadable: ' + (error.strerror or str(error))
        files[filename] = problem or 'ok'
    sums = parse_sums(data.pop(SUMS)) if SUMS in data else None
    if SUMS in files and files[SUMS] == 'ok' and sums is None:
        files[SUMS] = 'must list exactly the two modules and health script'
    for filename in FILES:
        if files[filename] != 'ok':
            continue
        if sums is None:
            files[filename] = 'unverified: SHA256SUMS is unusable'
        elif (hashlib.sha256(data[filename]).hexdigest() != sums[filename] or
              sums[filename] != PINS.get(filename)):
            files[filename] = 'does not match SHA256SUMS'
    return files, data


def loaded(root, name):
    return (root / 'sys/module' / name).exists()


def driver(root):
    values = {}
    for name in PARAMETERS:
        value = read(root / 'sys/module' / CAMERA[0] / 'parameters' / name)
        values[name] = int(value) if value is not None and value.isdigit() else value
    return values


def video_node(root):
    for name in sorted((root / 'sys/class/video4linux').glob('video*/name')):
        if read(name) == NAME:
            return '/dev/' + name.parent.name
    return None


def attempts(root, boot_id):
    """This boot's Stage D attempt entries, or None if they cannot be read."""
    if not boot_id:
        return None
    try:
        names = os.listdir(root / RUNNER_STATE)
    except FileNotFoundError:
        return []
    except OSError:
        return None
    return sorted(name for name in names if name.startswith(f'boot-{boot_id}-'))


def tuning_status(root, lstat):
    problem = directory_problem(root, str(Path(TUNING).parent), lstat)
    problem = problem or root_file(root / TUNING, lstat)
    if problem is not None:
        return problem
    with (root / TUNING).open('rb') as stream:
        data = stream.read(0x6ff5)
    if len(data) != 0x6ff4 or hashlib.sha256(data).hexdigest() != TUNING_SHA256:
        return 'unqualified tuning identity'
    return 'ok'


def inspect(root, lstat=os.lstat):
    compatible = root / COMPATIBLE
    release = read(root / OSRELEASE)
    files, data = verify(root, lstat)
    vermagic = {filename: module_release(data[filename]) if filename in data else None for _, filename in MODULES}
    boot_id = read(root / BOOT_ID)
    names = (LOADER[0], CAMERA[0], DIAGNOSTIC, *STAGE_C)
    report = {
        'board': compatible.is_file() and {BOARD, b'apple,t8132'} <= set(compatible.read_bytes().split(b'\0')),
        'kernel': release,
        'vermagic': vermagic,
        'kernel_match': release is not None and all(value == release for value in vermagic.values()),
        'files': files,
        'tuning': tuning_status(root, lstat),
        'firmware_crc': read(root / 'sys/module' / LOADER[0] / 'parameters/code_crc'),
        'files_verified': all(value == 'ok' for value in files.values()),
        'loaded': {name: loaded(root, name) for name in names},
        'driver': driver(root) if loaded(root, CAMERA[0]) else None,
        'video_node': video_node(root),
        'attempt_marker': os.path.lexists(root / STATE / 'attempt'),
        'held': os.path.lexists(root / STATE / 'hold'),
        'boot_id': boot_id,
        'boot_attempts': attempts(root, boot_id),
        'boot_unit_enabled': os.path.lexists(root / WANTS),
    }
    state = (report['driver'] or {}).get('state')
    if any(report['loaded'][name] for name in STAGE_C):
        report['state'] = 'stage c'
    elif report['loaded'][DIAGNOSTIC]:
        report['state'] = 'diagnostic'
    elif report['loaded'][CAMERA[0]]:
        report['state'] = 'loaded' if state in HEALTHY else 'dead' if state == 'dead' else 'failed'
    elif report['boot_attempts']:
        report['state'] = 'spent'
    elif report['attempt_marker']:
        report['state'] = 'struck'
    else:
        report['state'] = 'not loaded'
    return report


def describe(report):
    state = report['state']
    if report['held']:
        return 'off; nothing loads at boot until sudo m4-camera --enable' + (
            '; the loaded camera stays until power-off' if report['loaded'][CAMERA[0]] else '')
    if state == 'loaded':
        return f'current; {report["video_node"]} is {NAME}; the firmware starts at the first STREAMON'
    if state in ('dead', 'failed', 'spent'):
        if state == 'dead':
            what = 'DEAD: ' + str(report['driver']['dead_reason'])
        elif state == 'failed':
            what = f'the camera module did not probe (state {report["driver"]["state"]})'
        else:
            what = "this boot's load attempt did not finish"
        return f'{what}; the camera is unavailable until a physical power-off; the rest of the system is fine'
    if state == 'stage c':
        return 'Stage C is loaded; power off before using the camera'
    if state == 'diagnostic':
        return 'the Stage D diagnostic is loaded; power off before using the camera'
    if state == 'struck':
        return 'a boot stopped while loading; nothing loads at boot until sudo m4-camera --start'
    if not report['board']:
        return 'not a J713 MacBook Air; nothing loads'
    if not report['files_verified']:
        return 'will not load: the installed files are missing or do not match SHA256SUMS; reinstall'
    if not report['kernel_match']:
        return "will not load: the running kernel differs from the modules' vermagic; rebuild and reinstall"
    if report['tuning'] != 'ok':
        return 'will not load: owner tuning ' + report['tuning'] + '; run omarchy-m4-update and inspect its owner-data message'
    if not report['boot_unit_enabled']:
        return 'not enabled at boot; sudo m4-camera --start loads it in the running session'
    return 'loads at the next boot'


def make_state(root, lstat):
    """Create the state directory, making each new directory entry durable."""
    problem = directory_problem(root, STATE, lstat, create=True)
    require(problem is None, 'Refusing persistent state: ' + str(problem))
    return root / STATE


def write_durably(path, text):
    # Replace the entry, never truncate through a symlink or hard link.
    fd, temporary = tempfile.mkstemp(prefix='.' + path.name + '-', dir=path.parent)
    temporary = Path(temporary)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(text.encode())
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        sync_directory(path.parent)
    finally:
        temporary.unlink(missing_ok=True)


def mark(root, boot_id, lstat):
    """Write the attempt marker and make it durable before any module loads."""
    write_durably(make_state(root, lstat) / 'attempt', f'boot_id={boot_id} loading the J713 camera modules\n')


def unmark(root, lstat):
    path = make_state(root, lstat) / 'attempt'
    if os.path.lexists(path):
        path.unlink()
        sync_directory(path.parent)


def hold(root, held, lstat):
    """Turn the off switch on (write the hold file) or off (remove it), durably."""
    path = make_state(root, lstat) / 'hold'
    if held:
        write_durably(path, 'the camera stays unloaded until sudo m4-camera --enable\n')
    elif os.path.lexists(path):
        path.unlink()
        sync_directory(path.parent)


@contextlib.contextmanager
def runner_lock(root, lstat):
    """Take the runner's lock: the same directory, file and nonblocking flock."""
    state = root / RUNNER_STATE
    try:
        info = lstat(state)
    except FileNotFoundError:
        os.mkdir(state, 0o700)
    else:
        require(stat.S_ISDIR(info.st_mode) and info.st_uid == 0,
                'Refusing an existing state directory not owned by root.')
        require(not info.st_mode & 0o022, 'Refusing a state directory writable by group or others.')
    problem = root_file(state / 'lock', lstat)
    require(problem in (None, 'missing'), 'Refusing the runner lock: ' + str(problem))
    fd = os.open(state / 'lock', os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise Refused('Another camera or Stage D runner is active.') from None
        yield state
    finally:
        os.close(fd)


def bound(root, device, name, sleep):
    """Wait up to 5 s for a driver link, then require the expected driver, as the runner."""
    link = root / 'sys/bus/platform/devices' / device / 'driver'
    for _ in range(WAIT_STEPS):
        if link.is_symlink():
            return os.path.basename(os.readlink(link)) == name
        sleep(WAIT_STEP)
    return False


def sequence(root, state, boot_id, runner, lstat, sleep):
    """The runner's load from the loader insmod onwards; returns the video node."""
    directory = root / INSTALLED
    if not loaded(root, LOADER[0]):
        log('loading the per-boot Stage D overlay, DARTs and mailbox')
        require(runner(['insmod', str(directory / LOADER[1])]) == 0,
                f'Loading {LOADER[1]} failed; see journalctl -k -b for the reason.', Failed)
    require(read(root / 'sys/module' / LOADER[0] / 'parameters/setup_result') == '0',
            'Overlay failed. Power off before another attempt.', Failed)
    crc = read(root / 'sys/module' / LOADER[0] / 'parameters/code_crc')
    require(crc is not None and int(crc, 0) in QUALIFIED_FIRMWARE,
            'Unqualified ISP firmware CRC; camera stays unloaded.', Failed)
    for dart in DARTS:
        require(bound(root, dart + '.iommu', 'apple-dart', sleep), f'DART {dart} did not bind.', Failed)
    require(bound(root, MAILBOX, 'apple-mailbox', sleep), 'The camera mailbox did not bind.', Failed)
    # Stage D scans this exact pattern. From here on a failure spends the boot.
    attempt = state / f'boot-{boot_id}-1'
    os.mkdir(attempt, 0o700)
    sync_directory(state)
    log(f'loading camera attempt=/{attempt.relative_to(root)}; firmware starts at first STREAMON')

    def spent(condition, message):
        require(condition, message + ' ' + SPENT, Failed)

    for module in MEDIA:
        spent(runner(['modprobe', module]) == 0, f'Loading {module} failed.')
    spent(runner(['sync']) == 0, 'sync failed.')
    try:
        (root / COMPACT).write_text('1\n')
    except OSError as error:
        spent(False, f'Memory compaction failed ({error.strerror or error}).')
    spent(runner(['insmod', str(directory / CAMERA[1])]) == 0,
          f'Loading {CAMERA[1]} failed; see journalctl -k -b for the reason.')
    state_value = read(root / 'sys/module' / CAMERA[0] / 'parameters/state')
    spent(state_value == 'probed', f'Camera probe failed (state {state_value}). Keep the module loaded.')
    node = video_node(root)
    spent(node is not None, 'Camera video node missing.')
    try:
        mode = lstat(root / node.lstrip('/')).st_mode
    except FileNotFoundError:
        mode = 0
    spent(stat.S_ISCHR(mode), 'Camera node is not a character device.')
    log('camera loaded: ' + node)
    return node


def load_guards(report, mode):
    require(not report['held'],
            'The camera is switched off. Run sudo m4-camera --enable to switch it back on.')
    require(mode == 'start' or not report['attempt_marker'],
            'A previous boot stopped while loading the camera, so this boot loads nothing. '
            'Run sudo m4-camera --start to try again in the running session.')
    require(not any(report['loaded'][name] for name in STAGE_C),
            'Stage C is present. Power off before using the camera.')
    if report['loaded'][DIAGNOSTIC] or report['loaded'][CAMERA[0]]:
        require(not report['loaded'][DIAGNOSTIC] and report['driver']['state'] in HEALTHY,
                'Diagnostic or camera is already loaded. Keep it loaded until power-off.')
        return True
    require(report['board'], 'This is not a J713 MacBook Air.')
    require(report['files_verified'], 'The installed camera files are not usable: ' + ', '.join(
        f'{name} {problem}' for name, problem in report['files'].items() if problem != 'ok'))
    require(report['tuning'] == 'ok', 'Owner tuning is unavailable or unqualified: ' + report['tuning'])
    require(report['kernel'] == RELEASE and report['kernel_match'], f'Kernel mismatch: the running kernel is {report["kernel"]}, the modules are for '
            + ', '.join(f'{name} {value}' for name, value in report['vermagic'].items()) + '. Rebuild and reinstall.')
    return False


def load(root, report, runner, lstat, sleep, mode):
    load_guards(report, mode)
    with runner_lock(root, lstat) as state:
        # Serialize even the current-camera path: it may clear a stale strike.
        if load_guards(inspect(root, lstat), mode):
            if mode == 'start':
                unmark(root, lstat)
            return 'current'
        boot_id = read(root / BOOT_ID)
        require(boot_id, 'The boot id is unreadable.')
        # Recheck under the lock: a runner may have loaded or attempted meanwhile.
        require(not loaded(root, DIAGNOSTIC) and not loaded(root, CAMERA[0]),
                'Diagnostic or camera is already loaded. Keep it loaded until power-off.')
        require(attempts(root, boot_id) == [],
                'Firmware start already attempted in this boot. Physical power-off is required.')
        require(runner(['python3', str(root / INSTALLED / HEALTH)]) == 0,
                'Host health check failed; no camera modules loaded.')
        mark(root, boot_id, lstat)
        try:
            result = sequence(root, state, boot_id, runner, lstat, sleep)
        except Exception:
            unmark(root, lstat)
            raise
        # An interruption may leave an insmod child alive. Keep its strike.
        unmark(root, lstat)
        return result


def run_command(command):
    return subprocess.run(command, check=False).returncode


def run(root, mode, runner=run_command, lstat=os.lstat, sleep=time.sleep):
    if mode in ('disable', 'enable'):
        hold(root, mode == 'disable', lstat)
    report = inspect(root, lstat)
    if mode == 'disable':
        report['status'] = ('off; the loaded camera stays until power-off' if report['loaded'][CAMERA[0]]
                            else 'off; nothing loads at boot')
        return report
    if mode in ('boot', 'start'):
        result = load(root, report, runner, lstat, sleep, mode)
        report = inspect(root, lstat)
        report['status'] = 'current' if result == 'current' else (
            f'loaded; {result} is {NAME}; the firmware starts at the first STREAMON; '
            'about 599 MiB stay held until power-off')
    else:  # inspect or enable
        report['status'] = describe(report)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--boot', action='store_true', help='load the camera; only m4-j713-camera.service runs this')
    action.add_argument('--start', action='store_true', help='load the camera now, in the running session')
    action.add_argument('--disable', action='store_true', help='switch the camera off: nothing loads until --enable')
    action.add_argument('--enable', action='store_true', help='switch the camera back on')
    parser.add_argument('--root', type=Path, default=Path('/'), help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    mode = next((name for name in ('boot', 'start', 'disable', 'enable') if getattr(args, name)), 'inspect')
    if mode != 'inspect' and os.geteuid() != 0:
        print('m4-camera: run with sudo', file=sys.stderr)
        return 1
    try:
        report = run(args.root, mode)
    except Refused as error:
        print('m4-camera: refused: ' + str(error), file=sys.stderr)
        return 1
    except Failed as error:
        print('m4-camera: failed: ' + str(error), file=sys.stderr)
        return 1
    except (OSError, ValueError) as error:
        print('m4-camera: error: ' + str(error), file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
