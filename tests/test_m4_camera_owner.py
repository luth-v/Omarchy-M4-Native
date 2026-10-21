# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Luthfi Hizb
"""Owner-local tuning extraction uses synthetic records and fake APFS readers."""
from contextlib import contextmanager
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('camera_owner', ROOT / 'tools/m4-camera-owner.py')
OWNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(OWNER)


def record():
    data = bytearray(OWNER.SIZE)
    struct.pack_into('>8I', data, 0, 0x22240106, 0x20, 0, 0, 0x20000000, 0x12340000, 0, 0x3c00000)
    struct.pack_into('<I', data, 0x6fd4, 0x958)
    return bytes(data)


class OwnerTuningTests(unittest.TestCase):
    def setUp(self):
        self.data = record()
        self.pin = hashlib.sha256(self.data).hexdigest()
        for patch in (mock.patch.object(OWNER, 'QUALIFIED', ({'size': OWNER.SIZE, 'sha256': self.pin},)),
                      mock.patch.object(OWNER, 'SHA256', self.pin)):
            patch.start()
            self.addCleanup(patch.stop)

    def test_exact_record_and_identical_universal_slices(self):
        daemon = self.data.ljust(0x8000, b'\0') * 2
        self.assertEqual(OWNER.extract(daemon), self.data)

    def test_unqualified_record_is_refused(self):
        with mock.patch.object(OWNER, 'QUALIFIED', ({'size': OWNER.SIZE, 'sha256': '0' * 64},)):
            with self.assertRaisesRegex(ValueError, 'Unqualified J713 tuning identity'):
                OWNER.extract(self.data)

    def test_missing_truncated_unaligned_wrong_sensor_and_conflicting_records(self):
        bad = bytearray(self.data)
        bad[100] ^= 1
        sensor = bytearray(self.data)
        struct.pack_into('<I', sensor, 0x6fd4, 0x959)
        for data in (b'', self.data[:-1], b'x' + self.data,
                     self.data.ljust(0x8000, b'\0') + bad, sensor):
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                OWNER.extract(data)

    def test_daemon_size_limit(self):
        with mock.patch.object(OWNER, 'LIMIT', 10), self.assertRaisesRegex(ValueError, 'size limit'):
            OWNER.extract(self.data)

    def metadata(self, volumes=None, snapshots=None):
        volumes = volumes if volumes is not None else [
            {'index': 0, 'role': 1, 'encrypted': False, 'unknown_incompat': 0, 'snapshot_count': 2}]
        snapshots = snapshots if snapshots is not None else [(0, 12), (0, 13)]
        return b'\n'.join(json.dumps(row).encode() for row in [
            {'type': 'container', 'checkpoint_xid': 14},
            *[{'type': 'volume', **row} for row in volumes],
            *[{'type': 'snapshot', 'volume': index, 'xid': xid} for index, xid in snapshots]])

    @contextmanager
    def partition(self, device):
        self.assertRegex(device, OWNER.INTERNAL)
        yield 42

    def reader(self, metadata=None, devices=None, cat=None):
        calls = []
        def run(args, **kwargs):
            calls.append((args, kwargs))
            if args[0] == 'lsblk':
                return json.dumps({'blockdevices': devices if devices is not None else [
                    {'type': 'part', 'fstype': 'apfs', 'path': '/dev/nvme0n1p2'}]}).encode()
            self.assertEqual(args[0], OWNER.HELPER)
            self.assertTrue(kwargs['unprivileged'])
            self.assertEqual(kwargs['pass_fds'], (42,))
            self.assertGreater(kwargs['timeout'], 0)
            self.assertLessEqual(kwargs['timeout'], OWNER.CALL)
            if args[1] == 'inspect':
                self.assertEqual(args[2:], ['42'])
                self.assertEqual(kwargs['limit'], OWNER.LIST_LIMIT)
                return metadata if metadata is not None else self.metadata()
            self.assertEqual(args[1], 'cat')
            self.assertEqual(args[2], '42')
            self.assertEqual(args[-1], '/' + OWNER.SOURCE)
            self.assertEqual(kwargs['limit'], OWNER.LIMIT)
            return cat(args) if cat else self.data
        return run, calls

    def test_selection_snapshot_then_live_order_and_identical_records_dedupe(self):
        run, calls = self.reader()
        daemons = OWNER.apfs_daemons(run, self.partition)
        with mock.patch.object(OWNER.platform, 'system', return_value='Linux'), \
             mock.patch.object(OWNER, 'apfs_daemons', return_value=daemons):
            self.assertEqual(OWNER.owner_record(),
                             (self.data, '/dev/nvme0n1p2 volume=0 view=snapshot:12; skipped sources=0'))
        self.assertEqual([(c[0][3], c[0][4]) for c in calls if c[0][1] == 'cat'],
                         [('0', '12'), ('0', '13'), ('0', '0')])
        self.assertEqual([c[0][0] for c in calls], ['lsblk'] + [OWNER.HELPER] * 4)

    def test_conflicting_qualified_records_across_views_and_in_one_daemon_refuse(self):
        changed = bytearray(self.data)
        changed[100] ^= 1
        other = bytes(changed)
        pins = ({'size': OWNER.SIZE, 'sha256': self.pin},
                {'size': OWNER.SIZE, 'sha256': hashlib.sha256(other).hexdigest()})
        for candidates in ([self.data, other], [self.data, self.data.ljust(0x8000, b'\0') + other]):
            with mock.patch.object(OWNER.platform, 'system', return_value='Linux'), \
                 mock.patch.object(OWNER, 'QUALIFIED', pins), \
                 mock.patch.object(OWNER, 'apfs_daemons', return_value=iter(
                     (f'view={n}', data) for n, data in enumerate(candidates))):
                with self.assertRaisesRegex(OWNER.ConflictingRecords, 'Conflicting'):
                    OWNER.owner_record()

    def test_bytes_and_bytearray_yield_immutable_identical_records(self):
        for daemon in (self.data, bytearray(self.data), bytearray(self.data.ljust(0x8000, b'\0') * 2)):
            result = OWNER.extract(daemon)
            self.assertIs(type(result), bytes)
            self.assertEqual(result, self.data)

    def test_encrypted_and_non_system_volumes_are_skipped(self):
        metadata = self.metadata([
            {'index': 0, 'role': 64, 'encrypted': True, 'unknown_incompat': 64, 'snapshot_count': 999},
            {'index': 1, 'role': 1, 'encrypted': True, 'snapshot_count': 99},
            {'index': 2, 'role': 16, 'encrypted': False, 'snapshot_count': 99},
            {'index': 3, 'role': 1, 'encrypted': False, 'unknown_incompat': 0, 'snapshot_count': 0}], [])
        run, calls = self.reader(metadata)
        self.assertEqual(list(OWNER.apfs_daemons(run, self.partition)),
                         [('/dev/nvme0n1p2 volume=3 view=live', self.data)])
        self.assertEqual(calls[-1][0][3:5], ['3', '0'])

    def test_incomplete_duplicate_invalid_or_excessive_snapshots_refuse(self):
        for metadata in (self.metadata(snapshots=[]), self.metadata(snapshots=[(0, 12), (0, 12)]),
                         self.metadata(snapshots=[(0, 12), (0, 15)]),
                         self.metadata(snapshots=[(0, 12), (99, 13)]),
                         b'{"type":"volume"}', self.metadata(snapshots=[(0, True)])):
            run, calls = self.reader(metadata)
            with self.subTest(metadata=metadata), mock.patch('sys.stderr', new_callable=io.StringIO):
                self.assertEqual(list(OWNER.apfs_daemons(run, self.partition)), [])
            self.assertEqual(len(calls), 2, 'incomplete metadata never reaches cat')
        metadata = self.metadata([{'index': 0, 'role': 1, 'encrypted': False, 'unknown_incompat': 0,
                                   'snapshot_count': 65}], [(0, n) for n in range(1, 66)])
        with self.assertRaises(ValueError):
            OWNER.system_views(metadata.replace(b'"checkpoint_xid": 14', b'"checkpoint_xid": 100'))

    def test_only_internal_ssd_apfs_partitions_are_opened_once(self):
        devices = [{'type': 'disk', 'path': '/dev/sda', 'children': [
            {'type': 'part', 'fstype': 'apfs', 'path': '/dev/sda2'}]}]
        devices += [{'type': 'part', 'fstype': fs, 'path': path} for path, fs in [
            ('/dev/nvme1n1p2', 'apfs'), ('/dev/mapper/x', 'apfs'), ('/dev/nvme0n1p0', 'apfs'),
            ('/dev/nvme0n1p3', 'ext4'), (None, 'apfs'),
            ('/dev/nvme0n1p2', 'apfs'), ('/dev/nvme0n1p2', 'apfs')]]
        run, calls = self.reader(devices=devices)
        with mock.patch.object(self, 'partition', wraps=self.partition) as opened:
            self.assertEqual(len(list(OWNER.apfs_daemons(run, opened))), 3)
        opened.assert_called_once_with('/dev/nvme0n1p2')

    def test_open_partition_rejects_external_symlinks_wrong_inode_and_identity_change(self):
        from unittest.mock import MagicMock
        block = SimpleNamespace(st_mode=stat.S_IFBLK, st_rdev=0x10302)
        node = MagicMock()
        node.__truediv__.return_value.is_file.return_value = True
        node.__truediv__.return_value.read_text.return_value = '259:2'
        node.resolve.return_value.parent.name = 'nvme0n1'
        def path(name):
            if name == '/sys/class/block':
                parent = MagicMock()
                parent.__truediv__.return_value = node
                return parent
            if name == '/sys/class/block/nvme0n1/removable':
                removable = MagicMock()
                removable.read_text.return_value = '0'
                return removable
            return Path(name)
        with mock.patch.object(OWNER, 'Path', side_effect=path), \
             mock.patch.object(OWNER, 'internal_ssd', return_value=True), \
             mock.patch.object(OWNER.fcntl, 'fcntl', return_value=43) as duplicate, \
             mock.patch.object(OWNER.os, 'lstat', return_value=block), \
             mock.patch.object(OWNER.os, 'open', return_value=42) as opened, \
             mock.patch.object(OWNER.os, 'fstat', return_value=block), \
             mock.patch.object(OWNER.os, 'close') as closed, \
             mock.patch.object(OWNER.os, 'major', return_value=259), \
             mock.patch.object(OWNER.os, 'minor', return_value=2):
            with OWNER.open_partition('/dev/nvme0n1p2') as fd:
                self.assertEqual(fd, 43)
            flags = opened.call_args.args[1]
            self.assertEqual(flags, OWNER.os.O_RDONLY | OWNER.os.O_NOFOLLOW | OWNER.os.O_CLOEXEC | OWNER.os.O_NONBLOCK)
            duplicate.assert_called_once_with(42, OWNER.fcntl.F_DUPFD_CLOEXEC, 3)
            self.assertEqual(closed.call_args_list, [mock.call(42), mock.call(43)])
            for invalid in ('/dev/sda2', '/dev/nvme1n1p2', '/dev/nvme0n1p0'):
                with self.assertRaises(ValueError), OWNER.open_partition(invalid):
                    self.fail('opened disallowed device')
            with mock.patch.object(OWNER.os, 'lstat', return_value=SimpleNamespace(st_mode=stat.S_IFLNK)):
                with self.assertRaises(ValueError), OWNER.open_partition('/dev/nvme0n1p2'):
                    self.fail('opened symlink')
            with mock.patch.object(OWNER.os, 'fstat', return_value=SimpleNamespace(st_mode=stat.S_IFREG)):
                with self.assertRaises(ValueError), OWNER.open_partition('/dev/nvme0n1p2'):
                    self.fail('opened regular file')
            with mock.patch.object(OWNER.os, 'fstat', return_value=SimpleNamespace(st_mode=stat.S_IFBLK, st_rdev=1)):
                with self.assertRaises(ValueError), OWNER.open_partition('/dev/nvme0n1p2'):
                    self.fail('opened changed inode')
            node.resolve.return_value.parent.name = 'nvme1n1'
            with self.assertRaises(ValueError), OWNER.open_partition('/dev/nvme0n1p2'):
                self.fail('opened external parent')

    def test_one_time_budget_covers_lsblk_inspection_and_every_read(self):
        now, timeouts = [0.0], []
        run, _ = self.reader()
        def slow(args, timeout=None, **kwargs):
            timeouts.append((args[1], timeout))
            result = run(args, timeout=timeout, **kwargs)
            now[0] += 60
            return result
        with self.assertRaisesRegex(ValueError, 'time budget'):
            list(OWNER.apfs_daemons(slow, self.partition, lambda: now[0]))
        self.assertEqual(timeouts, [('--json', 30), ('inspect', 120), ('cat', 120), ('cat', 60)])

    def test_helper_refusals_and_process_failures(self):
        def missing(args):
            if args[4] == '12':
                raise OWNER.ReaderRefusal(21, args, stderr=b'REFUSAL PATH_LOOKUP')
            return self.data
        run, _ = self.reader(cat=missing)
        refused = OWNER.Refusals()
        with mock.patch('sys.stderr', new_callable=io.StringIO) as errors:
            self.assertEqual([data for _, data in OWNER.apfs_daemons(run, self.partition, refuse=refused)],
                             [self.data, self.data])
            self.assertEqual(errors.getvalue(), '')
            refused.report()
        self.assertIn('PATH_LOOKUP', errors.getvalue())
        for code in (20, 21, 22, -11):
            def failure(args):
                raise OWNER.ReaderRefusal(code, args, stderr=b'REFUSAL TEST_FAILURE')
            run, _ = self.reader(cat=failure)
            refused = OWNER.Refusals()
            with self.subTest(code=code), mock.patch('sys.stderr', new_callable=io.StringIO) as errors:
                self.assertEqual(list(OWNER.apfs_daemons(run, self.partition, refuse=refused)), [])
                refused.report()
            self.assertIn('TEST_FAILURE', errors.getvalue())
        # An inspect refusal cannot be mistaken for an empty successful sweep.
        run, _ = self.reader()
        def partial(args, **kwargs):
            if args[1] == 'inspect':
                raise OWNER.ReaderRefusal(20, args, stderr=b'REFUSAL SNAPSHOT_SCAN_LIMIT')
            return run(args, **kwargs)
        refused = OWNER.Refusals()
        with mock.patch('sys.stderr', new_callable=io.StringIO) as errors:
            self.assertEqual(list(OWNER.apfs_daemons(partial, self.partition, refuse=refused)), [])
            refused.report()
        self.assertIn('SNAPSHOT_SCAN_LIMIT', errors.getvalue())

    def test_a_later_timeout_cannot_return_an_earlier_record(self):
        def interrupted(**kwargs):
            yield 'synthetic source', self.data
            raise subprocess.TimeoutExpired(['reader'], 120)
        with mock.patch.object(OWNER.platform, 'system', return_value='Linux'), \
             mock.patch.object(OWNER, 'apfs_daemons', side_effect=interrupted):
            with self.assertRaises(subprocess.TimeoutExpired):
                OWNER.owner_record()

    def test_reader_output_stderr_time_exit_codes_and_inherited_descriptor_are_bounded(self):
        writer = [sys.executable, '-c', 'import sys; sys.stdout.write("x" * 65536)']
        self.assertEqual(OWNER.command(writer, limit=65536), b'x' * 65536)
        with self.assertRaisesRegex(ValueError, 'output exceeds'):
            OWNER.command(writer, limit=4096)
        with self.assertRaisesRegex(ValueError, 'output exceeds'), mock.patch.object(OWNER, 'ERROR_LIMIT', 1024):
            OWNER.command([sys.executable, '-c', 'import sys; sys.stderr.write("x" * 65536)'])
        with self.assertRaises(subprocess.TimeoutExpired):
            OWNER.command([sys.executable, '-c', 'import time; time.sleep(30)'], timeout=0.1)
        for code in (20, 21, 22, 24):
            with self.assertRaises(OWNER.ReaderRefusal) as caught:
                OWNER.command([sys.executable, '-c',
                               f'import sys; sys.stderr.write("REFUSAL TEST_REASON"); sys.exit({code})'])
            self.assertEqual(caught.exception.returncode, code)
            self.assertEqual(str(caught.exception), 'APFS reader refused: TEST_REASON')
        with tempfile.TemporaryFile() as source:
            source.write(b'synthetic')
            source.flush()
            self.assertEqual(OWNER.command([sys.executable, '-c',
                f'import os; os.write(1, os.pread({source.fileno()}, 9, 0))'],
                pass_fds=(source.fileno(),)), b'synthetic')

    def test_refused_partitions_before_and_after_the_main_container_do_not_hide_record(self):
        devices = [{'type': 'part', 'fstype': 'apfs', 'path': f'/dev/nvme0n1p{n}'} for n in range(1, 5)]
        current = []
        @contextmanager
        def partition(device):
            current[:] = [device]
            yield 42
        read, _ = self.reader(devices=devices)
        def run(args, **kwargs):
            if args[0] == OWNER.HELPER and args[1] == 'inspect' and current[0] != '/dev/nvme0n1p2':
                raise OWNER.ReaderRefusal(21, args, stderr=b'REFUSAL CONTAINER_INCOMPAT')
            return read(args, **kwargs)
        real = OWNER.apfs_daemons
        with mock.patch.object(OWNER.platform, 'system', return_value='Linux'), \
             mock.patch.object(OWNER, 'apfs_daemons', side_effect=lambda **kw: real(run, partition, **kw)), \
             mock.patch('sys.stderr', new_callable=io.StringIO) as errors:
            self.assertEqual(OWNER.owner_record(),
                             (self.data, '/dev/nvme0n1p2 volume=0 view=snapshot:12; skipped sources=3'))
        self.assertEqual(errors.getvalue(), '')

    def test_volume_refusals_and_incomplete_snapshots_skip_only_their_source(self):
        volumes = [{'index': 0, 'role': 1, 'encrypted': False, 'unknown_incompat': 0, 'snapshot_count': 2},
                   {'index': 1, 'role': 1, 'encrypted': False, 'unknown_incompat': 64, 'snapshot_count': 0},
                   {'index': 2, 'role': 1, 'encrypted': False, 'unknown_incompat': 0, 'snapshot_count': 3}]
        metadata = self.metadata(volumes) + b'\n' + json.dumps(
            {'type': 'refusal', 'volume': 2, 'reason': 'SNAPSHOT_TREE_INIT'}).encode()
        def cat(args):
            if args[4] == '12':
                raise OWNER.ReaderRefusal(21, args, stderr=b'REFUSAL SNAPSHOT_INCOMPLETE')
            return self.data
        run, _ = self.reader(metadata=metadata, cat=cat)
        refused = OWNER.Refusals()
        with mock.patch('sys.stderr', new_callable=io.StringIO) as errors:
            self.assertEqual([data for _, data in OWNER.apfs_daemons(run, self.partition, refuse=refused)],
                             [self.data, self.data])
            refused.report()
        for reason in ('VOLUME_UNKNOWN_INCOMPAT', 'SNAPSHOT_TREE_INIT', 'SNAPSHOT_INCOMPLETE'):
            self.assertIn(reason, errors.getvalue())

    def test_no_qualified_record_summarizes_named_reasons_without_payload(self):
        run, _ = self.reader(cat=lambda args: b'private-unqualified-data')
        real = OWNER.apfs_daemons
        with mock.patch.object(OWNER.platform, 'system', return_value='Linux'), \
             mock.patch.object(OWNER, 'apfs_daemons', side_effect=lambda **kw: real(run, self.partition, **kw)), \
             mock.patch('sys.stderr', new_callable=io.StringIO) as errors:
            with self.assertRaisesRegex(ValueError, r'UNQUALIFIED_OR_MISSING_RECORD \(3\)') as caught:
                OWNER.owner_record()
        self.assertNotIn('private', errors.getvalue() + str(caught.exception))
        self.assertEqual(len(errors.getvalue().splitlines()), 3)
        self.assertIn('/dev/nvme0n1p2 volume=0 view=snapshot:12', errors.getvalue())

    def test_success_prints_one_source_summary_and_reuses_an_existing_record(self):
        volumes = [{'index': 0, 'role': 64, 'encrypted': True},
                   {'index': 1, 'role': 16, 'encrypted': False},
                   {'index': 2, 'role': 1, 'encrypted': False, 'unknown_incompat': 0, 'snapshot_count': 0}]
        run, _ = self.reader(metadata=self.metadata(volumes, []))
        real = OWNER.apfs_daemons
        for installed in (False, True):
            with self.subTest(installed=installed), \
                 mock.patch.object(OWNER.platform, 'system', return_value='Linux'), \
                 mock.patch.object(OWNER, 'apfs_daemons',
                                   side_effect=lambda **kw: real(run, self.partition, **kw)) as sweep, \
                 mock.patch.object(OWNER, 'valid_installed', return_value=installed), \
                 mock.patch.object(OWNER, 'install_record') as install, \
                 mock.patch.object(OWNER.os, 'geteuid', return_value=0), \
                 mock.patch.object(OWNER.Path, 'read_bytes', return_value=b'apple,j713\0apple,t8132\0'), \
                 mock.patch.object(sys, 'argv', ['m4-camera-owner.py', '--install']), \
                 mock.patch('sys.stdout', new_callable=io.StringIO) as output, \
                 mock.patch('sys.stderr', new_callable=io.StringIO) as errors:
                self.assertEqual(OWNER.main(), 0)
            detail = ('existing qualified record' if installed else
                      '/dev/nvme0n1p2 volume=2 view=live; skipped sources=2')
            self.assertEqual(output.getvalue(), 'm4-camera-owner: qualified owner tuning ready; ' + detail + '\n')
            self.assertEqual(errors.getvalue(), '')
            if installed:
                sweep.assert_not_called()
                install.assert_not_called()
            else:
                install.assert_called_once_with(Path('/'), self.data)

    def test_unqualified_view_does_not_conflict_with_the_pinned_record(self):
        bad = bytearray(self.data)
        bad[100] ^= 1
        with mock.patch.object(OWNER.platform, 'system', return_value='Linux'), \
             mock.patch.object(OWNER, 'apfs_daemons', return_value=iter(
                 (f'view={n}', data) for n, data in enumerate([bad, self.data, self.data]))), \
             mock.patch('sys.stderr', new_callable=io.StringIO):
            self.assertEqual(OWNER.owner_record(), (self.data, 'view=1; skipped sources=1'))

    def test_refusal_logs_and_summary_are_bounded_and_do_not_emit_arbitrary_diagnostics(self):
        with mock.patch('sys.stderr', new_callable=io.StringIO) as errors:
            refused = OWNER.Refusals()
            for n in range(500):
                refused('REASON_' + str(n), 'synthetic source')
            refused('payload with spaces', 'synthetic source')
            self.assertEqual(errors.getvalue(), '')
            self.assertEqual(len(refused.diagnostics), 64)
            self.assertEqual(refused.total, 501)
            refused.report()
        self.assertEqual(len(errors.getvalue().splitlines()), 65)
        self.assertLessEqual(len(refused.counts), 64)
        self.assertLess(len(refused.summary()), 8000)
        self.assertNotIn('payload', refused.summary())
        self.assertEqual(OWNER.ReaderRefusal(21, [], stderr=b'REFUSAL ' + b'X' * 200).reason, 'EXIT_21')

    def test_parser_deadline_remains_a_hard_abort(self):
        def cat(args):
            raise OWNER.ReaderRefusal(24, args, stderr=b'REFUSAL DEADLINE')
        run, _ = self.reader(cat=cat)
        with self.assertRaises(OWNER.ReaderRefusal):
            list(OWNER.apfs_daemons(run, self.partition))

    def test_timeout_cleanup_never_uses_an_unbounded_wait_or_popen_context(self):
        child = mock.MagicMock()
        child.wait.side_effect = subprocess.TimeoutExpired(['reader'], 10)
        selector = mock.MagicMock()
        selector.__enter__.return_value = selector
        selector.select.side_effect = subprocess.TimeoutExpired(['reader'], 1)
        with mock.patch.object(OWNER.subprocess, 'Popen', return_value=child), \
             mock.patch.object(OWNER.selectors, 'DefaultSelector', return_value=selector), \
             mock.patch('sys.stderr', new_callable=io.StringIO):
            with self.assertRaises(subprocess.TimeoutExpired):
                OWNER.command(['reader'], timeout=1)
        child.kill.assert_called_once()
        self.assertGreaterEqual(child.wait.call_args.kwargs['timeout'], 0)
        self.assertLessEqual(child.wait.call_args.kwargs['timeout'], 10)
        child.__enter__.assert_not_called()
        child.stdout.close.assert_called_once()
        child.stderr.close.assert_called_once()

    def test_parser_launch_drops_user_groups_and_all_capability_sets(self):
        child = mock.MagicMock(returncode=0)
        selector = mock.MagicMock()
        selector.__enter__.return_value = selector
        selector.get_map.return_value = {}
        account = SimpleNamespace(pw_uid=65534, pw_gid=65534)
        with mock.patch.object(OWNER.subprocess, 'Popen', return_value=child) as popen, \
             mock.patch.object(OWNER.selectors, 'DefaultSelector', return_value=selector), \
             mock.patch.object(OWNER.pwd, 'getpwnam', return_value=account):
            self.assertEqual(OWNER.command(['reader', 'inspect', '42'], pass_fds=(42,), unprivileged=True), b'')
        self.assertEqual(popen.call_args.args[0], ['/usr/bin/setpriv', '--reuid=65534', '--regid=65534',
            '--clear-groups', '--no-new-privs', '--bounding-set=-all', '--inh-caps=-all',
            '--ambient-caps=-all', '--', 'reader', 'inspect', '42'])
        self.assertEqual(popen.call_args.kwargs['pass_fds'], (42,))
        self.assertTrue(popen.call_args.kwargs['close_fds'])

    def test_internal_ssd_requires_apple_ans_controller_not_just_nvme0_name(self):
        with tempfile.TemporaryDirectory() as temp:
            sysfs = Path(temp)
            driver = sysfs / 'bus/platform/drivers/nvme-apple'
            driver.mkdir(parents=True)
            namespace = sysfs / 'block/nvme0n1'
            namespace.mkdir(parents=True)
            (namespace / 'removable').write_text('0')
            controller = sysfs / 'devices/platform/ans/nvme/nvme0'
            controller.mkdir(parents=True)
            (namespace / 'device').symlink_to(controller)
            self.assertFalse(OWNER.internal_ssd(sysfs))
            (controller.parent.parent / 'driver').symlink_to(driver)
            self.assertTrue(OWNER.internal_ssd(sysfs))
            (namespace / 'removable').write_text('1')
            self.assertFalse(OWNER.internal_ssd(sysfs))

    def test_owner_record_uses_the_installed_macos_path(self):
        with mock.patch.object(OWNER.platform, 'system', return_value='Darwin'), \
             mock.patch.object(OWNER, 'read_regular', return_value=self.data) as read:
            self.assertEqual(OWNER.owner_record(), (self.data, '/usr/sbin/appleh16camerad'))
            read.assert_called_once_with(Path('/usr/sbin/appleh16camerad'), OWNER.LIMIT)

    def test_missing_macos_never_downloads_or_guesses(self):
        with mock.patch.object(OWNER.platform, 'system', return_value='Linux'), \
             mock.patch.object(OWNER, 'apfs_daemons', return_value=iter(())):
            with self.assertRaisesRegex(ValueError, 'No readable macOS APFS'):
                OWNER.owner_record()

    def test_atomic_install_repeats_and_refuses_a_different_file_or_symlink(self):
        real_fstat = OWNER.os.fstat
        def info(fd):
            value = real_fstat(fd)
            return SimpleNamespace(st_mode=value.st_mode, st_uid=0, st_size=value.st_size)
        with tempfile.TemporaryDirectory() as temp, \
             mock.patch.object(OWNER, 'trusted_directory'), \
             mock.patch.object(OWNER.os, 'fstat', side_effect=info):
            root = Path(temp)
            target = root / OWNER.DESTINATION
            target.parent.mkdir(parents=True)
            OWNER.install_record(root, self.data)
            OWNER.install_record(root, self.data)
            self.assertTrue(OWNER.valid_installed(root))
            self.assertEqual(target.stat().st_mode & 0o777, 0o644)
            target.write_bytes(b'other owner file')
            with self.assertRaisesRegex(ValueError, 'refusing to replace'):
                OWNER.install_record(root, self.data)
            target.unlink()
            external = root / 'external'
            external.write_bytes(b'unchanged')
            target.symlink_to(external)
            with self.assertRaises(OSError):
                OWNER.install_record(root, self.data)
            self.assertEqual(external.read_bytes(), b'unchanged')

    def test_untrusted_directory_is_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(ValueError):
                OWNER.trusted_directory(Path(temp))


if __name__ == '__main__':
    unittest.main()
