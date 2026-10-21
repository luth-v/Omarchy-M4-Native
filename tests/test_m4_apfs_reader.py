# SPDX-License-Identifier: MIT
"""Build the helper and inspect an invented fixture, without devices or mounts.

M4_APFS_SOURCES names a directory containing the two pinned source archives.
CI downloads those sources, never a disk image. Public content fixtures remain
outside the repository and are exercised separately by tests/offline.py.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
READER = ROOT / 'video/j713/camera/apfs-reader'
SOURCES = {
    'apfs-fuse-66b86bd525e8cb90f9012543be89b1f092b75cf3':
        'e78e71e87d8ba182822ec34233189141b8816c9a3f700c9a22f6dc1480afee51',
    'lzfse-e634ca58b4821d9f3d560cdc6df5dec02ffc93fd':
        'ca98aa6644d44500e3315858daa747ce15bd06d49e3edb12a5458e5525e8ebdb',
}


class ReaderBuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cache = os.environ.get('M4_APFS_SOURCES')
        if not cache:
            raise unittest.SkipTest('M4_APFS_SOURCES not set; pinned reader sources needed')
        for command in ('cmake', 'c++', 'patch'):
            if not shutil.which(command):
                raise AssertionError('Missing reader build dependency: ' + command)
        cls.scratch = tempfile.TemporaryDirectory(prefix='m4-apfs-test-')
        cls.addClassCleanup(cls.scratch.cleanup)
        cls.root = Path(cls.scratch.name)
        cls.log = (cls.root / 'build.log').open('w+')
        cls.addClassCleanup(cls.log.close)
        for name, pin in SOURCES.items():
            archive = Path(cache) / (name + '.tar.gz')
            if hashlib.sha256(archive.read_bytes()).hexdigest() != pin:
                raise AssertionError('Unpinned reader source: ' + name)
            with tarfile.open(archive) as source:
                source.extractall(cls.root, filter='data')
        apfs, lzfse = [cls.root / name for name in SOURCES]
        shutil.copytree(lzfse, apfs / '3rdparty/lzfse', dirs_exist_ok=True)
        with (READER / 'apfslib-readonly.patch').open() as patch:
            cls.run_build(['patch', '--batch', '--fuzz=0', '-p1', '-d', str(apfs)], stdin=patch)
        for test_images, name in (('OFF', 'production'), ('ON', 'fixtures')):
            build = cls.root / name
            cls.run_build(['cmake', '-S', str(READER), '-B', str(build),
                           '-DAPFS_SOURCE_DIR=' + str(apfs), '-DM4_APFS_TEST_IMAGES=' + test_images])
            cls.run_build(['cmake', '--build', str(build), '--target', 'm4-apfs-reader', '-j2'])
        cls.run_build(['cmake', '--build', str(cls.root / 'fixtures'), '--target', 'fixture-layout'])
        cls.image = cls.root / 'empty.apfs'
        cls.run_build([str(cls.root / 'fixtures/fixture-layout'), str(cls.image)])

    @classmethod
    def run_build(cls, args, **kwargs):
        result = subprocess.run(args, stdout=cls.log, stderr=subprocess.STDOUT, timeout=120, **kwargs)
        if result.returncode:
            cls.log.seek(0)
            raise AssertionError('Reader build failed: ' + str(args) + '\n' + cls.log.read()[-10000:])

    def test_valid_synthetic_container_and_bad_nxsb(self):
        helper = str(self.root / 'fixtures/m4-apfs-reader')
        before = self.image.read_bytes()
        result = subprocess.run([helper, '--image', 'inspect', str(self.image)], capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout),
                         {'type': 'container', 'checkpoint_xid': 1, 'block_size': 4096, 'incompat': 0})
        self.assertEqual(self.image.read_bytes(), before)
        bad = self.root / 'bad.apfs'
        bad.write_bytes(bytes(32768))
        result = subprocess.run([helper, '--image', 'inspect', str(bad)], capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 21)
        self.assertIn(b'REFUSAL NOT_APFS', result.stderr)
        self.assertEqual(result.stdout, b'')

    def test_installed_build_has_no_image_mode_or_regular_descriptor_access(self):
        helper = str(self.root / 'production/m4-apfs-reader')
        result = subprocess.run([helper, '--image', 'inspect', str(self.image)], capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 21)
        with self.image.open('rb') as stream:
            result = subprocess.run([helper, 'inspect', str(stream.fileno())],
                                    pass_fds=(stream.fileno(),), capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 21)
        self.assertIn(b'REFUSAL INPUT_NOT_BLOCK', result.stderr)
        with self.image.open('r+b') as stream:
            result = subprocess.run([helper, 'inspect', str(stream.fileno())],
                                    pass_fds=(stream.fileno(),), capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 21)
        self.assertIn(b'REFUSAL INPUT_NOT_READ_ONLY', result.stderr)
        self.assertEqual(result.stdout, b'')
        self.assertFalse(list(self.root.rglob('apfs-fuse')), 'no FUSE executable was built')

    def test_out_of_band_build_refuses_either_unpinned_archive_before_building(self):
        for damaged in SOURCES:
            sources = self.root / ('bad-' + damaged)
            sources.mkdir()
            for name in SOURCES:
                shutil.copyfile(Path(os.environ['M4_APFS_SOURCES']) / (name + '.tar.gz'),
                                sources / (name + '.tar.gz'))
            (sources / (damaged + '.tar.gz')).write_bytes(b'unpinned input')
            output = sources / 'output'
            result = subprocess.run(['bash', str(READER / 'build.sh'), str(sources), str(output)],
                                    capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn((damaged + '.tar.gz: FAILED').encode(), result.stdout)
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
