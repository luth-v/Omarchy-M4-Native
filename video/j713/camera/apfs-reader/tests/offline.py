#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fixture regression tests. Payloads stay in pipes/memory; logs contain metadata."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--helper', type=Path, default=ROOT / 'build/m4-apfs-reader')
parser.add_argument('--fixtures', type=Path, required=True)
parser.add_argument('--layout', type=Path, default=ROOT / 'build/fixture-layout')
parser.add_argument('--scratch', type=Path, required=True)
args = parser.parse_args()
helper = args.helper.resolve()
layout = json.loads(subprocess.check_output([str(args.layout.resolve())]))
scratch = args.scratch
scratch.mkdir(parents=True, exist_ok=True)
checks = 0


def run(arguments, code=0, reason=None, digest=None):
    global checks
    command = [str(helper), *map(str, arguments)]
    result = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, timeout=20)
    print('$', ' '.join(command))
    print('exit', result.returncode)
    print(result.stderr.decode('utf-8', 'replace').rstrip() or 'stderr: (empty)')
    assert result.returncode == code, (result.returncode, code)
    if 'inspect' in arguments:
        print(result.stdout.decode('ascii').rstrip())
    else:
        actual = hashlib.sha256(result.stdout).hexdigest()
        print('stdout_length', len(result.stdout), 'stdout_sha256', actual)
        if digest:
            assert actual == digest
        if code:
            assert not result.stdout, 'Refusal returned file bytes'
    if reason:
        assert ('REFUSAL ' + reason).encode() in result.stderr
    checks += 1
    return result


dfvfs = args.fixtures / 'dfvfs-apfs.raw'
content = args.fixtures / 'apfs-content-padded.apfs'
for image in [dfvfs, content]:
    rows = [json.loads(line) for line in run(['--image', 'inspect', image]).stdout.splitlines()]
    assert rows[0]['block_size'] == 4096
    assert rows[0]['checkpoint_xid'] == (4 if image == dfvfs else 14)

for image, path, digest in [
        (dfvfs, '/passwords.txt', '02a2a6af2f1ecf4720d7d49d640f0d0a269a7ec733e41973bdd34f09dad0e252'),
        (content, '/plain.txt', '289af0a0b21ede77675f1173c65cc2388d3b26570dbaaa7f2506444e05abf86b'),
        (content, '/compressed.txt', '3f58a41850c1096de883ada14c98c2375a85b473c80ccbef03c9e72c113abc78')]:
    run(['--image', 'cat', image, '0', '0', path], digest=digest)
for path, reason in [('/symlink_to_beth', 'SYMLINK'), ('/missing', 'PATH_LOOKUP_ABSENT_OR_INVALID'),
                     ('/Dir1', 'NON_REGULAR_FILE'), ('/symlink_to_beth/child', 'SYMLINK'),
                     ('/../plain.txt', 'PATH_COMPONENT_REFUSED')]:
    run(['--image', 'cat', content, '0', '0', path], code=21, reason=reason)
run(['inspect', content], code=21, reason='INPUT_DESCRIPTOR')
run(['--image', 'cat', content, '100', '0', '/plain.txt'], code=21, reason='ARGUMENT_NUMBER_RANGE')
run(['--image', 'cat', content, '0', '999999', '/plain.txt'], code=21, reason='SNAPSHOT_FUTURE_XID')
link = scratch / 'input-symlink'
if link.is_symlink():
    link.unlink()
link.symlink_to(content.resolve())
run(['--image', 'inspect', link], code=21, reason='INPUT_OPEN')


def checksum(block):
    # APFS Fletcher-64 checksum excludes the eight checksum bytes themselves.
    words = struct.unpack_from('<' + 'I' * ((len(block) - 8) // 4), block, 8)
    a, b, modulus = 0, 0, 0xffffffff
    for word in words:
        a = (a + word) % modulus
        b = (b + a) % modulus
    low = modulus - ((a + b) % modulus)
    high = modulus - ((a + low) % modulus)
    struct.pack_into('<II', block, 0, low, high)


original = content.read_bytes()
assert original[438 * 4096 + 32:438 * 4096 + 36] == b'APSB'


def modified(name, changes):
    data = bytearray(original)
    touched = set()
    for block_index, offset, format_, value in changes:
        struct.pack_into(format_, data, block_index * 4096 + offset, value)
        touched.add(block_index)
    for block_index in touched:
        block = data[block_index * 4096:(block_index + 1) * 4096]
        checksum(block)
        data[block_index * 4096:(block_index + 1) * 4096] = block
    path = scratch / name
    # Preserve the public prefix and its advertised sparse zero-filled tail.
    path.write_bytes(data[:442 * 4096])
    with path.open('r+b') as stream:
        stream.truncate(len(data))
    return path


# A missing hardware keylocker must not block a plaintext volume. It is skipped,
# never decoded. Patch all known NXSB copies so the selected checkpoint sees it.
nxblocks = [n for n in range(9) if original[n * 4096 + 32:n * 4096 + 36] == b'NXSB']
keybag = modified('keylocker.apfs', [(n, layout['keylocker'], '<Q', 9999999) for n in nxblocks] +
                  [(n, layout['keylocker'] + 8, '<Q', 1) for n in nxblocks])
result = run(['--image', 'cat', keybag, '0', '0', '/plain.txt'],
             digest='289af0a0b21ede77675f1173c65cc2388d3b26570dbaaa7f2506444e05abf86b')
assert b'NOTE KEYLOCKER_SKIPPED' in result.stderr
encrypted = modified('encrypted-data.apfs', [(438, layout['role'], '<H', 64),
                     (438, layout['fs_flags'], '<Q', 0), (438, layout['fs_root'], '<Q', 9999999)])
run(['--image', 'inspect', encrypted])
run(['--image', 'cat', encrypted, '0', '0', '/plain.txt'], code=21, reason='ENCRYPTED_VOLUME')
encrypted_system = modified('encrypted-system.apfs', [(438, layout['role'], '<H', 1),
                            (438, layout['fs_flags'], '<Q', 0), (438, layout['fs_root'], '<Q', 9999999)])
run(['--image', 'inspect', encrypted_system])
run(['--image', 'cat', encrypted_system, '0', '0', '/plain.txt'], code=21, reason='ENCRYPTED_VOLUME')

# Mutate every checkpoint copy of the public fixture's decmpfs header. No
# fixture bytes are embedded in this source, only the format marker and limits.
headers, start = [], 0
while (offset := original.find(b'fpmc', start)) >= 0:
    headers.append(offset)
    start = offset + 4
assert headers
for name, field, format_, value, reason in [
        ('compression-overlong', 8, '<Q', struct.unpack_from('<Q', original, headers[0] + 8)[0] - 1, 'DECOMPRESSION_LENGTH'),
        ('compression-size', 8, '<Q', 2**64 - 1, 'FILE_SIZE_LIMIT'),
        ('compression-empty', 8, '<Q', 0, 'DECMPFS_SIZE_LIMIT'),
        ('compression-unsupported', 4, '<I', 13, 'UNSUPPORTED_COMPRESSION')]:
    image = modified(name + '.apfs', [(n // 4096, n % 4096 + field, format_, value) for n in headers])
    run(['--image', 'cat', image, '0', '0', '/compressed.txt'], code=21, reason=reason)

unrelated = modified('unrelated-tree.apfs', [(438, layout['fs_root'], '<Q', 9999999)])
run(['--image', 'inspect', unrelated])
unknown = modified('unknown-system-incompat.apfs', [(438, layout['role'], '<H', 1),
                   (438, layout['incompat'], '<Q', 1 | (1 << 63))])
result = run(['--image', 'inspect', unknown], reason='VOLUME_UNKNOWN_INCOMPAT')
assert json.loads(result.stdout.splitlines()[-1]) == {
    'type': 'refusal', 'volume': 0, 'reason': 'VOLUME_UNKNOWN_INCOMPAT'}
bad_tree = modified('invalid-btree-count.apfs', [(432, 36, '<I', 0xffffffff)])
run(['--image', 'cat', bad_tree, '0', '0', '/plain.txt'], code=21, reason='BTREE_ENTRY_COUNT')
bad_extent = modified('bad-file-extent.apfs', [])
# The file data block is deliberately truncated while container size stays in
# the NXSB. The header size check refuses this incomplete image before lookup.
with bad_extent.open('r+b') as stream:
    stream.truncate(347 * 4096 + 10)
run(['--image', 'cat', bad_extent, '0', '0', '/plain.txt'], code=21, reason='CONTAINER_SIZE')


def node(oid, xid, subtype, records, fixed=False):
    """Small synthetic root+leaf node, with ordinary physical addressing."""
    block = bytearray(4096)
    struct.pack_into('<QQII', block, 8, oid, xid, 0x40000002, subtype)
    stride = 4 if fixed else 8
    table_size = stride * len(records)
    struct.pack_into('<HHIHH', block, 32, 7 if fixed else 3, 0, len(records), 0, table_size)
    key_start, key_offset, value_offset = 56 + table_size, 0, 0
    for n, (key, value) in enumerate(records):
        value_offset += len(value)
        assert key_start + key_offset + len(key) < 4056 - value_offset
        block[key_start + key_offset:key_start + key_offset + len(key)] = key
        block[4056 - value_offset:4056 - value_offset + len(value)] = value
        if fixed:
            assert len(key) == 16 and len(value) == 16
            struct.pack_into('<HH', block, 56 + stride * n, key_offset, value_offset)
        else:
            struct.pack_into('<4H', block, 56 + stride * n, key_offset, len(key), value_offset, len(value))
        key_offset += len(key)
    struct.pack_into('<6I2Q', block, 4056, 0x10, 4096, 16 if fixed else 0, 16 if fixed else 0,
                     max(len(k) for k, _ in records), max(len(v) for _, v in records), len(records), 1)
    checksum(block)
    return block


def snapshot_fixture():
    data = bytearray(original[:520 * 4096])
    while len(data) < 520 * 4096:
        data.extend(bytes(min(4096, 520 * 4096 - len(data))))
    live = bytearray(data[438 * 4096:439 * 4096])
    root_oid = struct.unpack_from('<Q', live, layout['fs_root'])[0]
    # Two OMAP versions of the SAME virtual filesystem root exercise the
    # point-in-time lookup rather than merely reusing the live mapping.
    mappings = [(struct.pack('<QQ', root_oid, xid), struct.pack('<IIQ', 0, 4096, physical))
                for xid, physical in [(12, 504), (13, 505)]]
    data[503 * 4096:504 * 4096] = node(503, 14, 11, mappings, fixed=True)
    omap = bytearray(data[433 * 4096:434 * 4096])
    struct.pack_into('<Q', omap, 48, 503)
    checksum(omap)
    data[433 * 4096:434 * 4096] = omap
    snapshot_records, expected = [], {}
    for xid, superblock, fsblock, contentblock in [(12, 501, 504, 506), (13, 502, 505, 507)]:
        value = struct.pack('<5QIIH', 0, superblock, 0, 0, 2, 0, 0, len(f'fixture-{xid}') + 1)
        value += f'fixture-{xid}\0'.encode()
        snapshot_records.append((struct.pack('<Q', (1 << 60) | xid), value))
        fs = bytearray(data[432 * 4096:433 * 4096])
        struct.pack_into('<Q', fs, 16, xid)
        count = struct.unpack_from('<I', fs, 36)[0]
        key_start = 56 + struct.unpack_from('<H', fs, 42)[0]
        found = False
        for n in range(count):
            ko, kl, vo, vl = struct.unpack_from('<4H', fs, 56 + n * 8)
            key = struct.unpack_from('<Q', fs, key_start + ko)[0]
            if key == (8 << 60) | 18:
                assert kl == 16 and vl == 24
                struct.pack_into('<Q', fs, 4056 - vo + 8, contentblock)
                found = True
        assert found
        checksum(fs)
        data[fsblock * 4096:(fsblock + 1) * 4096] = fs
        payload = f'Synthetic snapshot {xid} data only.\n'.encode().ljust(35, b'.')
        assert len(payload) == 35
        data[contentblock * 4096:contentblock * 4096 + 35] = payload
        expected[xid] = hashlib.sha256(payload).hexdigest()
        snap = bytearray(live)
        struct.pack_into('<Q', snap, 16, xid)
        struct.pack_into('<H', snap, layout['role'], 1)
        checksum(snap)
        data[superblock * 4096:(superblock + 1) * 4096] = snap
    data[500 * 4096:501 * 4096] = node(500, 14, 16, snapshot_records)
    struct.pack_into('<H', live, layout['role'], 1)
    struct.pack_into('<Q', live, layout['snap_tree'], 500)
    struct.pack_into('<Q', live, layout['snap_count'], 2)
    checksum(live)
    data[438 * 4096:439 * 4096] = live
    path = scratch / 'synthetic-snapshots.apfs'
    path.write_bytes(data)
    with path.open('r+b') as stream:
        stream.truncate(len(original))
    return path, expected


snapshots, snapshot_hashes = snapshot_fixture()
rows = [json.loads(line) for line in run(['--image', 'inspect', snapshots]).stdout.splitlines()]
assert [row['xid'] for row in rows if row['type'] == 'snapshot'] == [12, 13]
for xid, digest in snapshot_hashes.items():
    run(['--image', 'cat', snapshots, '0', str(xid), '/plain.txt'], digest=digest)
run(['--image', 'cat', snapshots, '0', '11', '/plain.txt'], code=21, reason='SNAPSHOT_OMAP_OR_TREE_LOOKUP')

# A synthetic sealed extent tree exercises fext lookup with the public data
# block. This is parser coverage, not an Apple seal or Tahoe compatibility test.
sealed = modified('synthetic-sealed.apfs', [(438, layout['role'], '<H', 1),
                  (438, layout['incompat'], '<Q', 0x21), (438, layout['fext_tree'], '<Q', 510)])
fext = node(510, 14, 31, [(struct.pack('<QQ', 18, 0), struct.pack('<QQ', 4096, 347))])
with sealed.open('r+b') as stream:
    stream.seek(510 * 4096)
    stream.write(fext)
run(['--image', 'cat', sealed, '0', '0', '/plain.txt'],
    digest='289af0a0b21ede77675f1173c65cc2388d3b26570dbaaa7f2506444e05abf86b')
print('PASS', checks, 'helper fixture/refusal checks')
