# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Luthfi Hizb
"""J713 camera production sessions, memory, lifecycle and firmware gate."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CameraKernelTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('gcc'), 'no host GCC with sanitizer runtime')
    def test_production_kernel_contracts(self):
        result = subprocess.run(['bash', str(ROOT / 'video/j713/camera/kernel/tests/run.sh')],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / 'production'
            subprocess.run(['gcc', '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
                            '-fno-pie', '-no-pie', '-fsanitize=address,undefined',
                            '-I', str(ROOT / 'video/j713/camera/kernel/tests'),
                            str(ROOT / 'tests/j713_camera_production.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
