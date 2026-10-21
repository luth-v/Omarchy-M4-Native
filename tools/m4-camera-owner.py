#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Luthfi Hizb
"""Extract the qualified J713 tuning record from the owner's macOS camera daemon.

The macOS installer reads /usr/sbin/appleh16camerad directly. Linux updates
read the internal SSD's APFS partitions through a separate reader, without mounting or
writing them; removable and external disks are never opened. Neither path
executes the daemon or downloads data. Only the pinned record is installed.
"""
import argparse
from contextlib import contextmanager, ExitStack
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import pwd
import re
import selectors
import stat
import struct
import subprocess
import sys
import tempfile
import time

SOURCE = 'usr/sbin/appleh16camerad'
DESTINATION = 'usr/lib/firmware/apple/isp_2224_01XX.dat'
SIZE = 0x6ff4
SHA256 = '1e25d89b7acc3b62fcb64c5102503ba289c3cb000ca6abe812c3249ac6349f77'
LIMIT = 128 * 1024 * 1024
# The Air's internal SSD, as m4-root-identity.sh names it. Nothing else is parsed.
INTERNAL = re.compile(r'/dev/nvme0n1p[1-9][0-9]*')
HELPER = '/usr/lib/omarchy-m4/j713-camera/m4-apfs-reader'
LIST_LIMIT, ERROR_LIMIT = 1024 * 1024, 64 * 1024
MAX_VOLUMES, MAX_SNAPSHOTS = 100, 64
# One budget for every reader call in a run, so a slow or crafted container cannot
# hold up the update; each call also keeps its own cap.
BUDGET, CALL = 240, 120
# A qualified list, as with owner AudioDSP inputs. An unknown record is refused.
QUALIFIED = ({'size': SIZE, 'sha256': SHA256, 'macos': '26.6.2 / 25G83'},)


class ConflictingRecords(ValueError):
    pass


class ReaderRefusal(subprocess.CalledProcessError):
    @property
    def reason(self):
        match = re.search(rb'REFUSAL ([A-Z][A-Z_0-9]{0,95})(?:\s|$)', self.stderr or b'')
        return match[1].decode('ascii') if match else (
            'EXIT_SIGNAL_' + str(-self.returncode) if self.returncode < 0 else 'EXIT_' + str(self.returncode))

    def __str__(self):
        # Never log captured file data or arbitrary library diagnostics.
        return 'APFS reader refused: ' + self.reason


class Refusals:
    """Keep named reasons and bounded diagnostics, never reader payloads."""
    def __init__(self):
        self.counts = {}
        self.diagnostics = []
        self.total = 0

    def __call__(self, reason, source):
        if not isinstance(reason, str) or not re.fullmatch(r'[A-Z][A-Z_0-9]{0,95}', reason):
            reason = 'READER_FAILURE'
        if reason not in self.counts and len(self.counts) >= 63:
            reason = 'OTHER_REFUSALS'
        self.counts[reason] = self.counts.get(reason, 0) + 1
        if len(self.diagnostics) < 64:
            self.diagnostics.append((source[:160], reason))
        self.total += 1

    def report(self):
        for source, reason in self.diagnostics:
            print(f'm4-camera-owner: {source}: {reason}', file=sys.stderr)
        if self.total > len(self.diagnostics):
            print('m4-camera-owner: further source diagnostics suppressed', file=sys.stderr)

    def summary(self):
        return '; '.join(f'{reason} ({count})' for reason, count in sorted(self.counts.items()))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def extract(data):
    """Universal slices may repeat the same record; conflicting records fail closed."""
    require(len(data) <= LIMIT, 'Camera daemon exceeds the size limit')
    records = set()
    for offset in range(0, len(data) - SIZE + 1, 0x1000):
        header = struct.unpack_from('>8I', data, offset)
        if (header[0] != 0x22240106 or header[2:4] != (0, 0) or
                not header[4] or header[4] & 0x00ffffff or
                not header[5] or header[5] & 0xffff or
                header[6:] != (0, 0x3c00000)):
            continue
        record = bytes(data[offset:offset + SIZE])
        require(struct.unpack_from('<I', record, 0x6fd4)[0] == 0x958,
                'Unqualified J713 tuning sensor')
        records.add(record)
    if len(records) > 1:
        raise ConflictingRecords('Conflicting J713 tuning records in ' + SOURCE)
    require(records, 'Missing J713 tuning records in ' + SOURCE)
    record = records.pop()
    digest = hashlib.sha256(record).hexdigest()
    require(any(entry['size'] == len(record) and entry['sha256'] == digest for entry in QUALIFIED),
            'Unqualified J713 tuning identity ' + digest + '; camera stays unloaded')
    return record


def trusted_directory(path, create=False):
    for component in reversed((path, *path.parents)):
        if create and not component.exists() and not component.is_symlink():
            component.mkdir(mode=0o755)
        info = component.lstat()
        require(stat.S_ISDIR(info.st_mode) and info.st_uid == 0 and not info.st_mode & 0o022,
                'Untrusted owner-data directory: ' + str(component))


def read_regular(path, limit):
    trusted_directory(path.parent)
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_uid == 0 and
                not info.st_mode & 0o022 and info.st_size <= limit,
                'Unsafe owner input: ' + str(path))
        data = stream.read(limit + 1)
    require(len(data) <= limit, 'Owner input grew beyond its limit')
    return data


def valid_installed(root):
    try:
        data = read_regular(root / DESTINATION, SIZE)
        return len(data) == SIZE and hashlib.sha256(data).hexdigest() == SHA256
    except (OSError, ValueError):
        return False


def install_record(root, record):
    require(len(record) == SIZE and hashlib.sha256(record).hexdigest() == SHA256,
            'Unqualified tuning output')
    target = root / DESTINATION
    trusted_directory(target.parent, create=True)
    if target.exists() or target.is_symlink():
        require(read_regular(target, SIZE) == record,
                'Existing camera tuning differs; refusing to replace it')
        return
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=target.parent, prefix='.camera-', delete=False) as stream:
            temporary = stream.name
            os.fchmod(stream.fileno(), 0o644)
            stream.write(record)
            stream.flush()
            os.fsync(stream.fileno())
        require(not target.exists() and not target.is_symlink(), 'Tuning destination appeared')
        os.replace(temporary, target)
        temporary = None
        fd = os.open(target.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    finally:
        if temporary is not None:
            os.unlink(temporary)


def command(args, limit=LIMIT, timeout=CALL, pass_fds=(), unprivileged=False):
    """Bound both pipes in memory and kill the child on a cap or deadline."""
    deadline = time.monotonic() + timeout
    # Reserve a short reap interval inside the call's deadline.
    work_deadline = deadline - min(1, timeout / 10)
    output, errors = bytearray(), bytearray()
    if unprivileged:
        account = pwd.getpwnam('nobody')
        require(account.pw_uid != 0 and account.pw_gid != 0, 'Unsafe parser account')
        args = ['/usr/bin/setpriv', '--reuid=' + str(account.pw_uid), '--regid=' + str(account.pw_gid),
                '--clear-groups', '--no-new-privs', '--bounding-set=-all',
                '--inh-caps=-all', '--ambient-caps=-all', '--', *args]
    # Popen.__exit__ waits without a deadline, including after a failed kill.
    child = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, close_fds=True, pass_fds=pass_fds)
    try:
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(child.stdout, selectors.EVENT_READ, (output, limit))
                selector.register(child.stderr, selectors.EVENT_READ, (errors, ERROR_LIMIT))
                while selector.get_map():
                    left = work_deadline - time.monotonic()
                    if left <= 0:
                        raise subprocess.TimeoutExpired(args, timeout)
                    for event, _ in selector.select(min(left, 0.5)):
                        destination, cap = event.data
                        chunk = os.read(event.fd, min(65536, cap - len(destination) + 1))
                        if not chunk:
                            selector.unregister(event.fileobj)
                            continue
                        require(len(destination) + len(chunk) <= cap, 'Owner-data reader output exceeds its limit')
                        destination.extend(chunk)
            child.wait(timeout=max(0.001, work_deadline - time.monotonic()))
        except BaseException:
            child.kill()
            try:
                child.wait(timeout=max(0, min(10, deadline - time.monotonic())))
            except subprocess.TimeoutExpired:
                print('m4-camera-owner: reader termination pending after bounded wait', file=sys.stderr)
            raise
        if child.returncode:
            raise ReaderRefusal(child.returncode, args, stderr=bytes(errors))
    finally:
        child.stdout.close()
        child.stderr.close()
    return bytes(output)


def internal_ssd(sysfs=Path('/sys')):
    """Require the namespace's controller ancestry to use the Apple ANS driver."""
    namespace = sysfs / 'block/nvme0n1'
    if (namespace / 'removable').read_text().strip() != '0':
        return False
    controller = (namespace / 'device').resolve(strict=True)
    driver = sysfs / 'bus/platform/drivers/nvme-apple'
    return driver.is_dir() and any((node / 'driver').resolve() == driver
                                  for node in (controller, *controller.parents))


@contextmanager
def open_partition(device):
    """Validate sysfs identity before opening; pass this same read-only inode on."""
    require(isinstance(device, str) and INTERNAL.fullmatch(device),
            'Not an internal SSD partition: ' + str(device))
    node = Path('/sys/class/block') / Path(device).name
    require((node / 'partition').is_file() and node.resolve().parent.name == 'nvme0n1' and internal_ssd(),
            'Not a partition of the internal SSD')
    expected = (node / 'dev').read_text().strip()
    before = os.lstat(device)
    require(stat.S_ISBLK(before.st_mode), 'APFS input is not a block device')
    opened = os.open(device, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK)
    try:
        fd = fcntl.fcntl(opened, fcntl.F_DUPFD_CLOEXEC, 3)
    finally:
        os.close(opened)
    try:
        info = os.fstat(fd)
        require(stat.S_ISBLK(info.st_mode) and info.st_rdev == before.st_rdev and
                f'{os.major(info.st_rdev)}:{os.minor(info.st_rdev)}' == expected,
                'APFS device identity changed')
        # The helper checks NXSB and the container checksum inside its timeout,
        # before any APFS trees. No device read can block the Python supervisor.
        yield fd
    finally:
        os.close(fd)


def system_views(listing, refuse=None, source='APFS'):
    """Validate inspect rows; isolate refused volumes before snapshot/live reads."""
    refuse = refuse if refuse is not None else Refusals()
    lines = listing.splitlines()
    require(0 < len(lines) <= 1 + MAX_VOLUMES * (2 + MAX_SNAPSHOTS), 'APFS inspect row limit')
    rows = [json.loads(line) for line in lines]
    require(all(isinstance(row, dict) for row in rows) and rows[0].get('type') == 'container',
            'APFS inspect container missing')
    checkpoint = rows[0].get('checkpoint_xid')
    require(type(checkpoint) is int and 0 < checkpoint < 2**64, 'APFS inspect checkpoint')
    volumes, snapshots, refused = {}, {}, set()
    for row in rows[1:]:
        kind = row.get('type')
        require(kind in ('volume', 'snapshot', 'refusal'), 'APFS inspect row type')
        index = row.get('index' if kind == 'volume' else 'volume')
        require(type(index) is int and 0 <= index < MAX_VOLUMES, 'APFS inspect volume index')
        if kind == 'refusal':
            require(index not in refused, 'APFS inspect duplicate refusal')
            refused.add(index)
            refuse(row.get('reason'), f'{source} volume={index}')
        elif kind == 'volume':
            require(index not in volumes, 'APFS inspect duplicate volume')
            volumes[index] = row
        else:
            xid = row.get('xid')
            require(type(xid) is int and 0 < xid <= checkpoint, 'APFS inspect snapshot xid')
            candidates = snapshots.setdefault(index, [])
            require(len(candidates) < MAX_SNAPSHOTS and xid not in candidates,
                    'APFS inspect snapshot limit or duplicate')
            candidates.append(xid)
    require(set(snapshots) <= set(volumes), 'APFS inspect orphan snapshot')
    views = []
    for index, volume in sorted(volumes.items()):
        if index in refused:
            continue
        if volume.get('role') != 1 or volume.get('encrypted') is not False:
            refuse('ENCRYPTED_VOLUME' if volume.get('encrypted') is True else 'NON_SYSTEM_VOLUME',
                   f'{source} volume={index}')
            continue
        if volume.get('unknown_incompat') != 0:
            refuse('VOLUME_UNKNOWN_INCOMPAT', f'{source} volume={index}')
            continue
        count = volume.get('snapshot_count')
        candidates = snapshots.get(index, [])
        if type(count) is not int or count != len(candidates):
            refuse('SNAPSHOT_COUNT_MISMATCH', f'{source} volume={index}')
            continue
        views.extend((index, xid) for xid in [*candidates, 0])
    return views


def apfs_daemons(run=command, partition=open_partition, clock=time.monotonic, refuse=None):
    refuse = refuse if refuse is not None else Refusals()
    deadline = clock() + BUDGET

    def remaining(cap=CALL):
        left = deadline - clock()
        require(left > 0, 'APFS reading used its time budget')
        return min(left, cap)

    listing = json.loads(run(['lsblk', '--json', '--paths', '--output', 'PATH,TYPE,FSTYPE'],
                             limit=LIST_LIMIT, timeout=remaining(30)))

    def partitions(items):
        # lsblk's JSON is bounded above. A stack avoids recursion on malformed input.
        pending = list(items)
        seen = set()
        while pending:
            item = pending.pop(0)
            require(isinstance(item, dict), 'Invalid partition listing')
            pending.extend(item.get('children') or [])
            device = item.get('path')
            if (item.get('type') == 'part' and item.get('fstype') == 'apfs' and
                    isinstance(device, str) and INTERNAL.fullmatch(device) and device not in seen):
                seen.add(device)
                yield device

    for device in partitions(listing.get('blockdevices') or []):
        remaining()
        with ExitStack() as opened:
            try:
                fd = opened.enter_context(partition(device))
            except (OSError, ValueError):
                refuse('PARTITION_OPEN_OR_IDENTITY', device)
                continue
            try:
                try:
                    metadata = run([HELPER, 'inspect', str(fd)], limit=LIST_LIMIT,
                                   timeout=remaining(), pass_fds=(fd,), unprivileged=True)
                except ReaderRefusal as error:
                    if error.returncode == 24:
                        raise
                    refuse(error.reason, device)
                    continue
                try:
                    views = system_views(metadata, refuse, device)
                except ValueError:
                    refuse('INSPECT_INVALID', device)
                    continue
                for index, xid in views:
                    source = f'{device} volume={index} view=' + (f'snapshot:{xid}' if xid else 'live')
                    try:
                        yield source, run([HELPER, 'cat', str(fd), str(index), str(xid), '/' + SOURCE],
                                          limit=LIMIT, timeout=remaining(), pass_fds=(fd,), unprivileged=True)
                    except ReaderRefusal as error:
                        if error.returncode == 24:
                            raise
                        refuse(error.reason, source)
            except OSError:
                refuse('READER_EXECUTION', device)
    remaining()


def owner_record():
    """Return the qualified bytes and a concise source/skip description."""
    if platform.system() == 'Darwin':
        return extract(read_regular(Path('/') / SOURCE, LIMIT)), '/' + SOURCE
    refuse, records = Refusals(), set()
    selected = None
    for source, daemon in apfs_daemons(refuse=refuse):
        try:
            records.add(extract(daemon))
            if selected is None:
                selected = source
        except ConflictingRecords:
            raise
        except ValueError:
            refuse('UNQUALIFIED_OR_MISSING_RECORD', source)
        if len(records) > 1:
            raise ConflictingRecords('Conflicting qualified J713 tuning records across APFS views')
    if records:
        return records.pop(), f'{selected}; skipped sources={refuse.total}'
    refuse.report()
    raise ValueError('No readable macOS APFS camera daemon; camera stays unloaded' +
                     ('; ' + refuse.summary() if refuse.counts else ''))


def add_to_firmware(package):
    """Transport only the qualified owner-local record through the installer package."""
    from gravity_firmware.core import FWFile
    name = 'apple/isp_2224_01XX.dat'
    try:
        record, _ = owner_record()
        package.add_file(name, FWFile(name, record))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print('m4-camera-owner: ' + str(error), file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--install', action='store_true', required=True)
    parser.parse_args()
    try:
        require(os.geteuid() == 0, 'Owner tuning installation requires root')
        tags = Path('/sys/firmware/devicetree/base/compatible').read_bytes().split(b'\0')
        require(b'apple,j713' in tags and b'apple,t8132' in tags, 'This is not a J713/T8132')
        source = 'existing qualified record'
        if not valid_installed(Path('/')):
            record, source = owner_record()
            install_record(Path('/'), record)
        print('m4-camera-owner: qualified owner tuning ready; ' + source)
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print('m4-camera-owner: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
