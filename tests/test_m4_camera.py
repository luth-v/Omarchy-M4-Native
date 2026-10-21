# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Luthfi Hizb
"""The boot-time camera load: the runner's guards, one attempt per boot, one strike after a boot dies.

Everything runs against a fake --root. Commands, ownership, character devices and
waits are injected; nothing loads a module or touches the real system.
"""
import configparser
import contextlib
import fcntl
import hashlib
import importlib.util
import io
import json
import multiprocessing
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

DRIVER = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('m4_camera', DRIVER / 'tools/m4-camera.py')
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
TUNING_STATUS = MODULE.tuning_status

RELEASE = '7.1.9-m4-gpu-backport-20261008'
BOOT = '11111111-2222-3333-4444-555555555555'
PROVIDERS = tuple((dart + '.iommu', 'apple-dart') for dart in MODULE.DARTS) + ((MODULE.MAILBOX, 'apple-mailbox'),)
SPENT = 'unavailable until a physical power-off. The rest of the system is fine.'


def elf(modinfo):
    """A minimal ELF64 little-endian object: a .modinfo section and its name table."""
    names = b'\0.modinfo\0.shstrtab\0'
    shoff = 64 + len(modinfo) + len(names)
    header = b'\x7fELF\x02\x01\x01' + bytes(9) + struct.pack(
        '<HHIQQQIHHHHHH', 1, 183, 1, 0, 0, shoff, 0, 64, 0, 0, 64, 3, 2)

    def section(name, kind, offset, size):
        return struct.pack('<IIQQQQIIQQ', name, kind, 0, 0, offset, size, 0, 0, 1, 0)
    return (header + modinfo + names + bytes(64) + section(1, 1, 64, len(modinfo))
            + section(10, 3, 64 + len(modinfo), len(names)))


def module(name, release=RELEASE):
    return elf(b'license=GPL\0vermagic=' + release.encode() + b' SMP preempt mod_unload aarch64\0name='
               + name.encode() + b'\0')


class Air:
    """insmod, modprobe and sync against a fake sysfs; lstat, devices and sleep are faked too.

    Every file is root-owned unless listed in foreign. Paths in devices are character devices.
    """

    def __init__(self, root):
        self.root, self.events, self.commands = root, [], []
        self.fail, self.setup_result, self.probe, self.node = None, '0', 'probed', True
        self.providers, self.foreign, self.devices = list(PROVIDERS), set(), set()
        self.sleeps, self.on_sleep, self.lock_free_during_load = 0, None, []

    def lstat(self, path):
        info = os.lstat(path)
        mode = stat.S_IFCHR | 0o660 if Path(path) in self.devices else info.st_mode
        return SimpleNamespace(st_mode=mode, st_uid=1000 if Path(path) in self.foreign else 0)

    def sleep(self, seconds):
        assert seconds == 0.1
        self.sleeps += 1
        if self.on_sleep:
            self.on_sleep(self.sleeps)

    def snapshot(self):
        return {'marker': (self.root / MODULE.STATE / 'attempt').is_file(),
                'attempt': (self.root / MODULE.RUNNER_STATE / f'boot-{BOOT}-1').is_dir(),
                'compact': (self.root / MODULE.COMPACT).read_text()}

    def __call__(self, command):
        self.commands.append(command)
        self.events.append(('run', command, self.snapshot()))
        if self.fail in (command[0], Path(command[-1]).name):
            return 1
        if command == ['insmod', str(self.root / MODULE.INSTALLED / 'j713_isp_d_dt.ko')]:
            self.loader()
        elif command == ['insmod', str(self.root / MODULE.INSTALLED / 'j713_isp_cam.ko')]:
            # The runner's flock(1) must see the tool's lock while it loads.
            lock = self.root / MODULE.RUNNER_STATE / 'lock'
            self.lock_free_during_load.append(subprocess.run(
                ['flock', '--nonblock', str(lock), 'true'], check=False).returncode == 0)
            self.camera(self.probe)
        return 0

    def bind(self, device, driver):
        link = self.root / 'sys/bus/platform/devices' / device / 'driver'
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to('../../../bus/platform/drivers/' + driver)

    def loader(self):
        parameters = self.root / 'sys/module/j713_isp_d_dt/parameters'
        parameters.mkdir(parents=True)
        (parameters / 'setup_result').write_text(self.setup_result + '\n')
        (parameters / 'code_crc').write_text(str(0xaaafbaa6))
        for device, driver in self.providers:
            self.bind(device, driver)

    def camera(self, state, reason='none'):
        parameters = self.root / 'sys/module/j713_isp_cam/parameters'
        parameters.mkdir(parents=True)
        for name, value in (('state', state), ('dead_reason', reason), ('fw_starts', '0'), ('sessions', '0')):
            (parameters / name).write_text(value + '\n')
        if self.node:
            name = self.root / 'sys/class/video4linux/video1/name'
            name.parent.mkdir(parents=True)
            name.write_text(MODULE.NAME + '\n')
            (self.root / 'dev/video1').touch()
            self.devices.add(self.root / 'dev/video1')

    def runs(self):
        return [event[1] for event in self.events if event[0] == 'run']


class CameraBoot(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name).resolve()
        self.lib = self.root / MODULE.INSTALLED
        self.air = Air(self.root)
        self.put(MODULE.COMPATIBLE, b'apple,j713\0apple,t8132\0apple,arm-platform\0')
        self.put(MODULE.OSRELEASE, (RELEASE + '\n').encode())
        self.put(MODULE.BOOT_ID, (BOOT + '\n').encode())
        self.put(MODULE.COMPACT, b'0\n')
        # Five seconds after boot: the boot load has no uptime rule.
        self.put('proc/uptime', b'5.00 1.00\n')
        # video0 is the Apple decoder; the camera is found by its name.
        self.put('sys/class/video4linux/video0/name', b'apple-vdec\n')
        (self.root / 'dev').mkdir()
        (self.root / 'dev/video0').touch()
        for directory in ('sys/module', 'var/tmp', 'var/lib', 'etc/systemd/system/multi-user.target.wants'):
            (self.root / directory).mkdir(parents=True, exist_ok=True)
        (self.root / MODULE.WANTS).symlink_to('/etc/systemd/system/m4-j713-camera.service')
        self.install()
        self.put(MODULE.TUNING, b'qualified tuning')
        patcher = mock.patch.object(MODULE, 'TUNING_SHA256', hashlib.sha256(b'qualified tuning').hexdigest())
        patcher.start()
        self.addCleanup(patcher.stop)
        patcher = mock.patch.object(MODULE, 'tuning_status', return_value='ok')
        patcher.start()
        self.addCleanup(patcher.stop)
        real_fsync = os.fsync

        def fsync(fd):
            self.air.events.append(('fsync', Path(os.readlink(f'/proc/self/fd/{fd}'))))
            return real_fsync(fd)
        for patcher in (mock.patch.object(MODULE.os, 'fsync', side_effect=fsync),
                        mock.patch('sys.stderr', new_callable=io.StringIO)):
            patcher.start()
            self.addCleanup(patcher.stop)

    def put(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def install(self, loader=None, camera=None, sums=None):
        files = {'j713_isp_d_dt.ko': loader or module('j713_isp_d_dt'),
                 'j713_isp_cam.ko': camera or module('j713_isp_cam'),
                 'health-on-air.py': b'fake health script\n'}
        MODULE.PINS = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
        for name, data in files.items():
            self.put(f'{MODULE.INSTALLED}/{name}', data)
        if sums is None:
            sums = ''.join(f'{hashlib.sha256(data).hexdigest()}  {name}\n' for name, data in files.items())
        self.put(f'{MODULE.INSTALLED}/SHA256SUMS', sums.encode())

    def run_mode(self, mode):
        return MODULE.run(self.root, mode, self.air, self.air.lstat, self.air.sleep)

    def marker(self):
        return self.root / MODULE.STATE / 'attempt'

    def attempt(self, number=1):
        return self.root / MODULE.RUNNER_STATE / f'boot-{BOOT}-{number}'

    def log(self):
        return MODULE.sys.stderr.getvalue()

    def refused(self, mode, message, error=None):
        with self.assertRaisesRegex(error or MODULE.Refused, message):
            self.run_mode(mode)

    def assert_nothing_done(self):
        self.assertEqual(self.air.commands, [])
        self.assertFalse(os.path.lexists(self.marker()), 'a refusal is not a strike')
        self.assertFalse(os.path.lexists(self.attempt()))

    # Inspection

    def test_inspection_on_a_clean_root_changes_nothing(self):
        report = self.run_mode('inspect')
        self.assertEqual(report['status'], 'loads at the next boot')
        self.assertEqual(report['state'], 'not loaded')
        self.assertTrue(report['board'] and report['kernel_match'] and report['files_verified'])
        self.assertEqual(report['files'], {'SHA256SUMS': 'ok', 'j713_isp_d_dt.ko': 'ok', 'j713_isp_cam.ko': 'ok',
                                          'health-on-air.py': 'ok'})
        self.assertEqual(report['vermagic'], {'j713_isp_d_dt.ko': RELEASE, 'j713_isp_cam.ko': RELEASE})
        self.assertEqual((report['kernel'], report['boot_id'], report['boot_attempts']), (RELEASE, BOOT, []))
        self.assertFalse(any(report['loaded'].values()))
        self.assertEqual((report['driver'], report['video_node']), (None, None))
        self.assertEqual((report['attempt_marker'], report['held'], report['boot_unit_enabled']),
                         (False, False, True))
        self.assertEqual(self.air.events, [])
        self.assertFalse((self.root / MODULE.STATE).exists())
        self.assertFalse((self.root / MODULE.RUNNER_STATE).exists())

    def test_inspection_reports_the_driver_and_each_unusable_state(self):
        (self.root / MODULE.WANTS).unlink()
        self.assertTrue(self.run_mode('inspect')['status'].startswith('not enabled at boot'))
        self.attempt().mkdir(parents=True)
        report = self.run_mode('inspect')
        self.assertEqual((report['state'], report['boot_attempts']), ('spent', [f'boot-{BOOT}-1']))
        self.assertIn('physical power-off', report['status'])
        self.air.camera('dead', 'IPC command 0x100 timed out')
        report = self.run_mode('inspect')
        self.assertEqual(report['driver'], {'state': 'dead', 'dead_reason': 'IPC command 0x100 timed out',
                                            'fw_starts': 0, 'sessions': 0, 'colour': None, 'framing': None, 'fps': None})
        self.assertEqual(report['video_node'], '/dev/video1')
        self.assertEqual(report['status'], 'DEAD: IPC command 0x100 timed out; the camera is unavailable until '
                                           'a physical power-off; the rest of the system is fine')
        with mock.patch.object(MODULE.os, 'listdir', side_effect=PermissionError):
            self.assertIsNone(self.run_mode('inspect')['boot_attempts'], 'unreadable without root')
        self.assertEqual(self.air.commands, [])

    def test_inspection_explains_why_nothing_would_load(self):
        self.put(MODULE.OSRELEASE, b'7.2.0-other\n')
        self.assertIn('rebuild and reinstall', self.run_mode('inspect')['status'])
        (self.lib / 'j713_isp_cam.ko').unlink()
        report = self.run_mode('inspect')
        self.assertEqual(report['files']['j713_isp_cam.ko'], 'missing')
        self.assertIn('do not match SHA256SUMS', report['status'])

    # The boot load

    def test_boot_runs_the_runner_sequence_in_order(self):
        report = self.run_mode('boot')
        library = self.lib
        self.assertEqual(self.air.runs(), [
            ['python3', str(library / 'health-on-air.py')],
            ['insmod', str(library / 'j713_isp_d_dt.ko')],
            ['modprobe', 'videodev'], ['modprobe', 'videobuf2-v4l2'], ['modprobe', 'videobuf2-vmalloc'],
            ['sync'],
            ['insmod', str(library / 'j713_isp_cam.ko')]], 'no module parameters: the defaults are production')
        states = [event[2] for event in self.air.events if event[0] == 'run']
        self.assertEqual([state['marker'] for state in states], [False] + [True] * 6, 'health first, marker durable before any load')
        self.assertEqual([state['attempt'] for state in states], [False] * 2 + [True] * 5,
                         'the attempt directory exists before the media modules and the camera insmod')
        self.assertEqual([state['compact'] for state in states], ['0\n'] * 6 + ['1\n'],
                         'compaction after sync, before the camera insmod')
        self.assertEqual(self.air.lock_free_during_load, [False], "the runner's flock is held out")
        events = [event[:2] for event in self.air.events]
        first_run = next(index for index, event in enumerate(events)
                         if event[0] == 'run' and event[1][0] == 'insmod')
        state_dir = self.root / MODULE.STATE
        marker_sync = next(index for index, event in enumerate(events)
                           if event[0] == 'fsync' and event[1].parent == state_dir
                           and event[1].name.startswith('.attempt-'))
        self.assertLess(marker_sync, first_run)
        self.assertLess(events.index(('fsync', state_dir)), first_run)
        runner_state = self.root / MODULE.RUNNER_STATE
        synced = events.index(('fsync', runner_state))
        self.assertLess(first_run, synced)
        self.assertLess(synced, events.index(('run', ['modprobe', 'videodev'])))
        self.assertFalse(self.marker().exists(), 'removed once the load returned')
        self.assertTrue(self.attempt().is_dir())
        self.assertEqual(stat.S_IMODE(runner_state.stat().st_mode), 0o700)
        self.assertEqual(subprocess.run(['flock', '--nonblock', str(runner_state / 'lock'), 'true']).returncode, 0)
        self.assertEqual(report['state'], 'loaded')
        self.assertEqual(report['status'], 'loaded; /dev/video1 is FaceTime HD Camera (J713); the firmware starts '
                                           'at the first STREAMON; about 599 MiB stay held until power-off')
        self.assertIn('loading the per-boot Stage D overlay, DARTs and mailbox', self.log())
        self.assertIn(f'loading camera attempt=/{MODULE.RUNNER_STATE}/boot-{BOOT}-1;', self.log())
        self.assertIn('camera loaded: /dev/video1', self.log())

    def test_a_second_run_in_the_same_boot_is_current(self):
        self.run_mode('boot')
        self.air.commands.clear()
        for mode in ('boot', 'start'):
            self.assertEqual(self.run_mode(mode)['status'], 'current')
        self.assertEqual(self.air.commands, [])

    def test_already_loaded_and_probed_is_current_otherwise_refused(self):
        for state in MODULE.HEALTHY:
            with self.subTest(state=state):
                self.air.camera(state)
                self.assertEqual(self.run_mode('boot')['status'], 'current')
                shutil.rmtree(self.root / 'sys/module/j713_isp_cam')
                shutil.rmtree(self.root / 'sys/class/video4linux/video1')
        for state in ('dead', 'unbound'):
            with self.subTest(state=state):
                self.air.camera(state)
                self.refused('boot', 'already loaded. Keep it loaded until power-off')
                shutil.rmtree(self.root / 'sys/module/j713_isp_cam')
                shutil.rmtree(self.root / 'sys/class/video4linux/video1')
        (self.root / 'sys/module/j713_isp_d').mkdir()
        self.refused('start', 'Diagnostic or camera is already loaded')
        self.assert_nothing_done()

    def test_a_loaded_loader_is_not_inserted_twice(self):
        self.air.loader()
        self.run_mode('boot')
        self.assertEqual(self.air.runs()[1], ['modprobe', 'videodev'])
        self.assertEqual(len(self.air.runs()), 6)

    # One strike and the off switch

    def test_a_boot_that_died_while_loading_stops_the_next_boot(self):
        self.put(MODULE.STATE + '/attempt', b'boot_id=old loading\n')
        report = self.run_mode('inspect')
        self.assertEqual(report['state'], 'struck')
        self.assertIn('sudo m4-camera --start', report['status'])
        self.refused('boot', 'previous boot stopped while loading the camera')
        self.assertEqual(self.air.commands, [])
        self.assertTrue(self.marker().exists())
        self.assertFalse((self.root / MODULE.RUNNER_STATE).exists())

    def test_start_loads_in_the_session_and_clears_the_strike(self):
        self.put(MODULE.STATE + '/attempt', b'boot_id=old loading\n')
        self.assertEqual(self.run_mode('start')['state'], 'loaded')
        self.assertEqual(len(self.air.commands), 7)
        self.assertFalse(self.marker().exists())
        self.put(MODULE.STATE + '/attempt', b'boot_id=old loading\n')
        self.assertEqual(self.run_mode('start')['status'], 'current')
        self.assertFalse(self.marker().exists(), '--start on a working camera clears a stale strike')

    def hold(self):
        return self.root / MODULE.STATE / 'hold'

    def test_disable_stops_boot_and_start_until_enable(self):
        report = self.run_mode('disable')
        self.assertTrue(self.hold().is_file())
        self.assertEqual(stat.S_IMODE(self.hold().stat().st_mode), 0o600)
        self.assertEqual((report['held'], report['status']), (True, 'off; nothing loads at boot'))
        self.assertTrue(self.run_mode('inspect')['status'].startswith('off;'))
        for mode in ('boot', 'start'):
            with self.subTest(mode):
                self.refused(mode, 'switched off')
                self.assert_nothing_done()
        report = self.run_mode('enable')
        self.assertFalse(self.hold().exists())
        self.assertEqual((report['held'], report['status']), (False, 'loads at the next boot'))
        self.assertEqual(self.run_mode('boot')['state'], 'loaded')

    def test_disable_with_the_camera_loaded_says_it_stays_until_power_off(self):
        self.air.camera('idle')
        self.assertEqual(self.run_mode('disable')['status'], 'off; the loaded camera stays until power-off')
        self.assertTrue(self.run_mode('inspect')['status'].endswith('stays until power-off'))
        self.assertEqual(self.air.commands, [])

    def test_disable_and_enable_are_repeatable_and_keep_a_strike(self):
        for mode in ('disable', 'disable', 'enable', 'enable'):
            self.run_mode(mode)
            self.assertEqual(self.hold().exists(), mode == 'disable')
        self.put(MODULE.STATE + '/attempt', b'loading\n')
        self.run_mode('disable')
        self.assertEqual(self.run_mode('enable')['state'], 'struck')
        self.refused('boot', 'previous boot stopped')

    # Pinned inputs

    def test_only_the_j713(self):
        self.put(MODULE.COMPATIBLE, b'apple,j613\0apple,t8132\0')
        self.refused('boot', 'not a J713')
        self.assert_nothing_done()
        self.assertFalse((self.root / MODULE.RUNNER_STATE).exists())

    def test_the_running_kernel_must_equal_each_vermagic(self):
        cases = (('running kernel', lambda: self.put(MODULE.OSRELEASE, b'7.1.9-other\n')),
                 ('camera vermagic', lambda: self.install(camera=module('j713_isp_cam', '7.1.10-m4'))),
                 ('loader vermagic', lambda: self.install(loader=module('j713_isp_d_dt', RELEASE + '-x'))),
                 ('no vermagic', lambda: self.install(camera=elf(b'license=GPL\0'))),
                 ('not an ELF module', lambda: self.install(camera=b'not a module')))
        for name, change in cases:
            with self.subTest(name):
                change()
                self.assertTrue(self.run_mode('inspect')['files_verified'], 'only the kernel differs')
                self.refused('boot', 'Kernel mismatch')
                self.assert_nothing_done()
                self.put(MODULE.OSRELEASE, (RELEASE + '\n').encode())
                self.install()
        self.assertEqual(self.run_mode('boot')['state'], 'loaded')

    def test_the_files_must_match_their_sha256sums(self):
        loader, camera = module('j713_isp_d_dt'), module('j713_isp_cam')
        health = b'fake health script\n'
        good = [f'{hashlib.sha256(data).hexdigest()}  {name}\n'
                for name, data in (('j713_isp_d_dt.ko', loader), ('j713_isp_cam.ko', camera),
                                   ('health-on-air.py', health))]
        cases = (('changed module', lambda: self.put(MODULE.INSTALLED + '/j713_isp_d_dt.ko', loader + b'x'),
                  'j713_isp_d_dt.ko does not match SHA256SUMS'),
                 ('extra line', lambda: self.install(sums=''.join(good) + good[0].replace('d_dt', 'other')),
                  'exactly the two modules and health script'),
                 ('one line', lambda: self.install(sums=good[0]), 'exactly the two modules and health script'),
                 ('duplicate', lambda: self.install(sums=good[0] * 2 + good[1]), 'exactly the two modules and health script'),
                 ('binary mode', lambda: self.install(sums=good[0] + good[1].replace('  ', ' *')),
                  'exactly the two modules and health script'),
                 ('no final newline', lambda: self.install(sums=''.join(good)[:-1]), 'exactly the two modules and health script'),
                 ('missing module', lambda: (self.lib / 'j713_isp_cam.ko').unlink(), 'j713_isp_cam.ko missing'),
                 ('missing sums', lambda: (self.lib / 'SHA256SUMS').unlink(), 'SHA256SUMS missing'))
        for name, change, message in cases:
            with self.subTest(name):
                change()
                self.refused('boot', message)
                self.assert_nothing_done()
                self.install()
        self.install(sums=good[2] + good[1] + good[0])
        self.assertEqual(self.run_mode('boot')['state'], 'loaded', 'line order does not matter')

    def test_symlinked_or_foreign_files_are_refused(self):
        elsewhere = self.root / 'elsewhere'
        shutil.copytree(self.lib, elsewhere)
        for name in ('j713_isp_cam.ko', 'j713_isp_d_dt.ko', 'health-on-air.py', 'SHA256SUMS'):
            with self.subTest(symlink=name):
                (self.lib / name).unlink()
                (self.lib / name).symlink_to(elsewhere / name)
                self.refused('boot', f'{name} a symlink')
                self.assert_nothing_done()
                (self.lib / name).unlink()
                shutil.copy(elsewhere / name, self.lib / name)
            with self.subTest(foreign=name):
                self.air.foreign.add(self.lib / name)
                self.refused('boot', f'{name} not owned by root')
                self.assert_nothing_done()
                self.air.foreign.clear()
        self.air.foreign.add(self.lib)
        self.refused('boot', 'directory is a symlink or not owned by root')
        self.air.foreign.clear()
        shutil.rmtree(self.lib)
        self.lib.symlink_to(elsewhere)
        self.refused('boot', 'directory is a symlink or not owned by root')
        self.assert_nothing_done()

    # The runner's guards

    def test_stage_c_is_refused(self):
        for name in MODULE.STAGE_C:
            with self.subTest(name):
                (self.root / 'sys/module' / name).mkdir()
                self.refused('boot', 'Stage C is present. Power off before using the camera.')
                self.assert_nothing_done()
                (self.root / 'sys/module' / name).rmdir()

    def test_any_attempt_in_this_boot_is_refused(self):
        runner_state = self.root / MODULE.RUNNER_STATE
        runner_state.mkdir(mode=0o700)
        (runner_state / 'boot-other-boot-1').mkdir()
        for number, make, remove in ((1, Path.mkdir, Path.rmdir), (7, Path.mkdir, Path.rmdir),
                                     (2, lambda path: path.symlink_to('/missing'), Path.unlink)):
            with self.subTest(number=number):
                make(self.attempt(number))
                self.refused('start', 'Firmware start already attempted in this boot. Physical power-off')
                self.assertEqual(self.air.commands, [])
                self.assertFalse(self.marker().exists())
                remove(self.attempt(number))
        self.assertEqual(self.run_mode('boot')['state'], 'loaded', "another boot's attempt does not count")

    def test_a_held_runner_lock_is_refused(self):
        runner_state = self.root / MODULE.RUNNER_STATE
        runner_state.mkdir(mode=0o700)
        with open(runner_state / 'lock', 'w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.refused('boot', 'Another camera or Stage D runner is active.')
            self.assert_nothing_done()
        self.assertEqual(self.run_mode('boot')['state'], 'loaded')

    def test_a_foreign_or_symlinked_runner_state_directory_is_refused(self):
        runner_state = self.root / MODULE.RUNNER_STATE
        runner_state.symlink_to(self.root / 'var/tmp')
        self.refused('boot', 'state directory not owned by root')
        runner_state.unlink()
        runner_state.mkdir()
        self.air.foreign.add(runner_state)
        self.refused('boot', 'state directory not owned by root')
        self.assert_nothing_done()

    # Failures

    def test_health_failure_prevents_the_marker_and_all_module_loads(self):
        self.air.fail = 'health-on-air.py'
        self.refused('boot', 'Host health check failed')
        self.assertEqual(self.air.commands, [['python3', str(self.lib / 'health-on-air.py')]])
        self.assertFalse(self.marker().exists())
        self.assertFalse(self.attempt().exists())

    def test_start_cannot_clear_the_strike_of_an_active_load(self):
        original = self.air.camera

        def probing(state):
            original(state)
            self.assertTrue(self.marker().exists())
            self.refused('start', 'Another camera or Stage D runner is active')
            self.assertTrue(self.marker().exists(), 'insmod has not returned yet')
        self.air.camera = probing
        self.assertEqual(self.run_mode('boot')['state'], 'loaded')
        self.assertFalse(self.marker().exists())

    def test_guards_are_rechecked_under_the_lock(self):
        original = MODULE.runner_lock
        changes = (
            (MODULE.STATE + '/hold', b'off\n', 'switched off'),
            (MODULE.STATE + '/attempt', b'another load\n', 'previous boot stopped'),
            ('sys/module/j713_isp_c', None, 'Stage C is present'),
            (MODULE.INSTALLED + '/j713_isp_cam.ko', b'changed', 'does not match'),
        )
        for path, value, message in changes:
            with self.subTest(path=path):
                @contextlib.contextmanager
                def changed(root, lstat):
                    with original(root, lstat) as state:
                        if value is None:
                            (root / path).mkdir()
                        else:
                            self.put(path, value)
                        yield state
                with mock.patch.object(MODULE, 'runner_lock', changed):
                    self.refused('boot', message)
                self.assertEqual(self.air.commands, [])
                if value is None:
                    (self.root / path).rmdir()
                else:
                    (self.root / path).unlink()
                self.install()

    def test_keyboard_interrupt_keeps_the_strike(self):
        def interrupted(command):
            if command == ['insmod', str(self.lib / 'j713_isp_cam.ko')]:
                raise KeyboardInterrupt
            return self.air(command)
        with self.assertRaises(KeyboardInterrupt):
            MODULE.run(self.root, 'boot', interrupted, self.air.lstat, self.air.sleep)
        self.assertTrue(self.marker().exists())
        self.assertTrue(self.attempt().is_dir())
        self.refused('boot', 'previous boot stopped')

    def test_termination_leaves_a_durable_strike_for_the_next_boot(self):
        context = multiprocessing.get_context('fork')
        ready = context.Event()

        def child():
            def blocked(command):
                if command[0] == 'insmod':
                    ready.set()
                    # Stay alive until the service's SIGTERM.
                    import signal
                    signal.pause()
                return self.air(command)
            MODULE.run(self.root, 'boot', blocked, self.air.lstat, self.air.sleep)
        process = context.Process(target=child)
        process.start()
        try:
            self.assertTrue(ready.wait(5), 'the marker must be durable before insmod starts')
            process.terminate()
            process.join(5)
            self.assertEqual(process.exitcode, -15)
        finally:
            if process.is_alive():
                process.kill()
                process.join(5)
        self.assertTrue(self.marker().exists())
        self.put(MODULE.BOOT_ID, b'next-boot\n')
        self.refused('boot', 'previous boot stopped')
        self.assertEqual(self.air.commands, [])

    def test_writable_files_and_ancestors_are_refused(self):
        for path in (self.lib / 'j713_isp_cam.ko', self.lib, self.root / 'usr/lib/omarchy-m4'):
            with self.subTest(path=path):
                mode = path.stat().st_mode & 0o777
                path.chmod(mode | 0o022)
                self.refused('boot', 'writable by group or others')
                self.assert_nothing_done()
                path.chmod(mode)
        self.air.foreign.add(self.root / 'usr/lib/omarchy-m4')
        self.refused('boot', 'directory is a symlink or not owned by root')
        self.assert_nothing_done()

    def test_persistent_state_cannot_write_through_unsafe_directories(self):
        outside = self.root / 'outside'
        outside.mkdir()
        parent = self.root / 'var/lib/omarchy-m4'
        parent.symlink_to(outside)
        for mode in ('start', 'disable', 'enable'):
            self.refused(mode, 'Refusing persistent state')
        self.assertEqual(list(outside.iterdir()), [])
        self.assertFalse(any(command[0] == 'insmod' for command in self.air.commands))
        parent.unlink()
        parent.mkdir()
        parent.chmod(0o777)
        self.refused('disable', 'writable by group or others')
        parent.chmod(0o755)
        self.air.foreign.add(parent)
        self.refused('disable', 'not owned by root')

    def test_durable_writes_replace_links_without_overwriting_their_targets(self):
        victim = self.root / 'victim'
        victim.write_text('untouched\n')
        self.run_mode('disable')
        self.hold().unlink()
        os.link(victim, self.hold())
        self.run_mode('disable')
        self.assertEqual(victim.read_text(), 'untouched\n')
        self.run_mode('enable')
        os.link(victim, self.marker())
        self.run_mode('start')
        self.assertEqual(victim.read_text(), 'untouched\n')

    def test_unsafe_runner_lock_and_writable_state_are_refused(self):
        state = self.root / MODULE.RUNNER_STATE
        state.mkdir(mode=0o700)
        lock = state / 'lock'
        os.mkfifo(lock)
        self.refused('boot', 'Refusing the runner lock')
        lock.unlink()
        lock.symlink_to('/missing')
        self.refused('boot', 'Refusing the runner lock')
        lock.unlink()
        state.chmod(0o777)
        self.refused('boot', 'state directory writable by group or others')
        self.assert_nothing_done()

    def test_a_failed_loader_insmod_removes_the_marker_and_spends_nothing(self):
        self.air.fail = 'j713_isp_d_dt.ko'
        self.refused('boot', 'Loading j713_isp_d_dt.ko failed; see journalctl', MODULE.Failed)
        self.assertEqual(len(self.air.commands), 2)
        self.assertFalse(self.marker().exists(), 'a load that returned is not a strike')
        self.assertFalse(self.attempt().exists())

    def test_overlay_and_provider_failures_stop_before_the_attempt(self):
        cases = (('setup', lambda: setattr(self.air, 'setup_result', '-22'), 'Overlay failed', 0),
                 ('dart missing', lambda: self.air.providers.pop(1), 'DART 48e8e0000 did not bind', 50),
                 ('dart driver', lambda: self.air.providers.__setitem__(0, ('48e8c0000.iommu', 'other')),
                  'DART 48e8c0000 did not bind', 0),
                 ('mailbox missing', lambda: self.air.providers.pop(), 'The camera mailbox did not bind', 50),
                 ('mailbox driver', lambda: self.air.providers.__setitem__(3, (MODULE.MAILBOX, 'other')),
                  'The camera mailbox did not bind', 0))
        for name, change, message, sleeps in cases:
            with self.subTest(name):
                self.air = Air(self.root)
                change()
                self.refused('boot', message, MODULE.Failed)
                self.assertEqual(self.air.sleeps, sleeps, 'each provider gets 5 s')
                self.assertEqual(len(self.air.commands), 2)
                self.assertFalse(self.marker().exists())
                self.assertFalse(self.attempt().exists(), 'no attempt before the providers are bound')
                shutil.rmtree(self.root / 'sys/module/j713_isp_d_dt')
                shutil.rmtree(self.root / 'sys/bus')

    def test_a_late_provider_binding_is_waited_for(self):
        late = self.air.providers.pop(2)
        self.air.on_sleep = lambda count: self.air.bind(*late) if count == 7 else None
        self.assertEqual(self.run_mode('boot')['state'], 'loaded')
        self.assertEqual(self.air.sleeps, 7)

    def test_a_failed_media_module_spends_the_boot(self):
        self.air.fail = 'videobuf2-v4l2'
        self.refused('boot', 'Loading videobuf2-v4l2 failed. The camera is ' + SPENT, MODULE.Failed)
        self.assertEqual(self.air.runs()[-1], ['modprobe', 'videobuf2-v4l2'])
        self.assertEqual((self.root / MODULE.COMPACT).read_text(), '0\n')
        self.assertFalse(self.marker().exists())
        self.assertTrue(self.attempt().is_dir())
        self.refused('start', 'already attempted in this boot', MODULE.Refused)

    def test_a_failed_camera_insmod_removes_the_marker_and_says_so(self):
        self.air.fail = 'j713_isp_cam.ko'
        self.refused('boot', 'Loading j713_isp_cam.ko failed; see journalctl -k -b for the reason. The camera is '
                     + SPENT, MODULE.Failed)
        self.assertEqual(len(self.air.commands), 7)
        self.assertFalse(self.marker().exists(), 'a load that returned is not a strike')
        self.assertTrue(self.attempt().is_dir(), 'the boot is spent')
        self.assertNotIn('rmmod', [command[0] for command in self.air.commands])
        self.refused('start', 'already attempted in this boot')
        self.assertEqual(len(self.air.commands), 7, 'never retried within a boot')

    def test_a_probe_that_is_not_probed_fails_and_keeps_the_module(self):
        self.air.probe = 'unbound'
        self.refused('boot', r'Camera probe failed \(state unbound\). Keep the module loaded. The camera is '
                     + SPENT, MODULE.Failed)
        self.assertFalse(self.marker().exists())
        report = self.run_mode('inspect')
        self.assertEqual(report['state'], 'failed')
        self.assertIn('did not probe (state unbound)', report['status'])
        self.refused('start', 'already loaded. Keep it loaded until power-off')
        self.assertNotIn('rmmod', [command[0] for command in self.air.commands])

    def test_the_video_node_must_be_a_character_device(self):
        self.air.node = False
        self.refused('boot', 'Camera video node missing. The camera is ' + SPENT, MODULE.Failed)
        shutil.rmtree(self.root / 'sys/module')
        shutil.rmtree(self.root / 'sys/bus')
        (self.root / 'sys/module').mkdir()
        self.attempt().rmdir()
        self.air = Air(self.root)
        original = self.air.camera

        def regular_node(state):
            original(state)
            self.air.devices.clear()
        self.air.camera = regular_node
        self.refused('boot', 'Camera node is not a character device. The camera is ' + SPENT, MODULE.Failed)
        self.assertFalse(self.marker().exists())

    # Command line, unit and installer

    def test_the_command_line_needs_root_for_every_change(self):
        with mock.patch.object(MODULE.os, 'geteuid', return_value=1000), mock.patch.object(MODULE, 'run') as run:
            for flag in ('--boot', '--start', '--disable', '--enable'):
                self.assertEqual(MODULE.main([flag]), 1, flag)
                self.assertEqual(MODULE.main(['--root', str(self.root), flag]), 1, flag)
            run.assert_not_called()

    def test_the_command_line_prints_a_report_or_the_refusal(self):
        with mock.patch('sys.stdout', new_callable=io.StringIO) as stdout:
            self.assertEqual(MODULE.main(['--root', str(self.root)]), 0)
        # main() checks real ownership, so the status depends on who runs the tests.
        report = json.loads(stdout.getvalue())
        self.assertEqual((report['board'], report['kernel'], report['state']), (True, RELEASE, 'not loaded'))
        self.put(MODULE.STATE + '/hold', b'off\n')
        with mock.patch.object(MODULE.os, 'geteuid', return_value=0):
            self.assertEqual(MODULE.main(['--root', str(self.root), '--boot']), 1)
        self.assertIn('m4-camera: refused: The camera is switched off.', self.log())

    def test_vermagic_parser_reads_the_built_modules(self):
        for name in ('j713_isp_d_dt.ko', 'j713_isp_cam.ko'):
            path = Path(os.environ.get('M4_CAMERA_MODULES', DRIVER / 'video/j713/camera/kernel')) / name
            if not path.exists():
                self.skipTest('modules not built')
            self.assertEqual(MODULE.module_release(path.read_bytes()), RELEASE)
            if shutil.which('modinfo'):
                vermagic = subprocess.check_output(['modinfo', '-F', 'vermagic', str(path)], text=True)
                self.assertEqual(vermagic.split()[0], RELEASE)
        self.assertEqual(MODULE.module_release(module('x', 'a-b')), 'a-b')
        for data in (b'', b'\x7fELF\x01\x01', module('x')[:200], elf(b'vermagic=a\0vermagic=b\0')):
            self.assertIsNone(MODULE.module_release(data))

    def test_the_unit_runs_once_after_udev_and_the_speakers_and_never_blocks_login(self):
        parser = configparser.ConfigParser(interpolation=None, strict=True)
        parser.optionxform = str
        parser.read_string((DRIVER / 'packaging/omarchy-m4/omarchy-m4-camera/m4-j713-camera.service').read_text())
        self.assertEqual(parser.sections(), ['Unit', 'Service', 'Install'])
        unit, service = dict(parser['Unit']), dict(parser['Service'])
        self.assertEqual(unit['After'].split(), ['systemd-udev-trigger.service', 'm4-j713-audio-register.service'])
        self.assertEqual(unit['ConditionPathExists'], '!/' + MODULE.STATE + '/hold')
        self.assertEqual(set(unit), {'Description', 'After', 'ConditionPathExists'},
                         'no Requires, Wants or Before: nothing waits for the camera')
        self.assertEqual(service, {'Type': 'exec', 'RemainAfterExit': 'yes',
                                   'ExecStart': '/usr/bin/m4-camera --boot',
                                   'TimeoutStartSec': '120', 'RuntimeMaxSec': '120',
                                   'TimeoutStopSec': '5', 'Restart': 'no'})
        self.assertEqual(dict(parser['Install']), {'WantedBy': 'multi-user.target'})
        self.assertEqual(MODULE.WANTS, 'etc/systemd/system/multi-user.target.wants/m4-j713-camera.service')

    def test_unavailable_tuning_never_loads_or_writes_a_strike(self):
        with mock.patch.object(MODULE, 'tuning_status', return_value='missing'):
            self.refused('boot', 'Owner tuning is unavailable')
        self.assert_nothing_done()

    def test_tuning_is_rechecked_under_the_lock(self):
        with mock.patch.object(MODULE, 'tuning_status', side_effect=['ok', 'missing']):
            self.refused('boot', 'Owner tuning is unavailable')
        self.assert_nothing_done()

    def test_tuning_hash_size_permissions_and_symlinks(self):
        data = b'x' * 0x6ff4
        self.put(MODULE.TUNING, data)
        with mock.patch.object(MODULE, 'tuning_status', TUNING_STATUS), \
             mock.patch.object(MODULE, 'TUNING_SHA256', hashlib.sha256(data).hexdigest()):
            self.assertEqual(TUNING_STATUS(self.root, self.air.lstat), 'ok')
            self.put(MODULE.TUNING, data + b'x')
            self.assertIn('unqualified', TUNING_STATUS(self.root, self.air.lstat))
            target = self.root / MODULE.TUNING
            target.unlink()
            target.symlink_to('/missing')
            self.assertEqual(TUNING_STATUS(self.root, self.air.lstat), 'a symlink')
            self.refused('boot', 'Owner tuning is unavailable')
        self.assert_nothing_done()

    def test_unqualified_loader_crc_blocks_the_camera(self):
        loader = self.air.loader
        def unqualified():
            loader()
            self.put('sys/module/j713_isp_d_dt/parameters/code_crc', b'1')
        self.air.loader = unqualified
        self.refused('boot', 'Unqualified ISP firmware CRC', MODULE.Failed)
        self.assertEqual([c[0] for c in self.air.commands], ['python3', 'insmod'])
        self.assertFalse(self.attempt().exists())
        self.assertFalse(self.marker().exists())

    def test_replacing_both_a_module_and_its_manifest_cannot_bypass_the_pin(self):
        self.put(MODULE.INSTALLED + '/j713_isp_cam.ko', b'changed')
        sums = ''.join(f'{hashlib.sha256((self.lib / name).read_bytes()).hexdigest()}  {name}\n'
                       for name in MODULE.FILES)
        self.put(MODULE.INSTALLED + '/SHA256SUMS', sums.encode())
        self.refused('boot', 'does not match SHA256SUMS')
        self.assert_nothing_done()

    def test_package_installs_where_the_tool_looks(self):
        recipe = (DRIVER / 'packaging/omarchy-m4/omarchy-m4-camera/PKGBUILD').read_text()
        self.assertIn('/' + MODULE.INSTALLED, recipe)
        self.assertIn('/usr/bin/m4-camera', recipe)



if __name__ == '__main__':
    unittest.main(verbosity=2)
