# SPDX-License-Identifier: MIT
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
# An override exercises the exact retained unit before the repair.
UNIT = Path(os.environ.get('J713_GPU_SERVICE', ROOT / 'packaging/j713-startup/m4-gpu.service'))


class GraphicsStartupTests(unittest.TestCase):
    def pre_start(self, ready):
        commands = [line.split('=', 1)[1] for line in UNIT.read_text().splitlines()
                    if line.startswith('ExecStartPre=')]
        self.assertEqual(len(commands), 1)
        argv = shlex.split(commands[0])
        self.assertEqual(argv[0], '/usr/bin/udevadm')
        with tempfile.TemporaryDirectory() as temporary:
            helper = Path(temporary) / 'udevadm'
            helper.write_text('''#!/bin/sh
case "$1" in
  settle) echo 'Unrelated USB event still pending' >&2; exit 1 ;;
  wait)
    [ "$#" = 3 ] && [ "$2" = '--timeout=10' ] && [ "$3" = '/dev/dri/card0' ] || exit 64
    [ "$SCANOUT_READY" = yes ] && exit 0
    echo 'Scanout device did not initialize' >&2; exit 1 ;;
  *) exit 64 ;;
esac
''')
            helper.chmod(0o755)
            return subprocess.run([str(helper), *argv[1:]], text=True, capture_output=True,
                                  env={**os.environ, 'SCANOUT_READY': 'yes' if ready else 'no'})

    def test_ready_scanout_can_start_with_unrelated_device_pending(self):
        result = self.pre_start(True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_missing_scanout_still_fails(self):
        self.assertNotEqual(self.pre_start(False).returncode, 0)

    def test_failure_diagnostics_reach_the_journal(self):
        lines = UNIT.read_text().splitlines()
        self.assertFalse([line for line in lines if line.startswith(('StandardOutput=', 'StandardError='))])
        self.assertIn('ExecStart=/usr/bin/bash -x /usr/local/bin/m4-gpu-start', lines)
        post = [line.split('=', 1)[1] for line in lines if line.startswith('ExecStopPost=')]
        self.assertEqual(len(post), 1)
        self.assertTrue(post[0].startswith('-/usr/bin/sh -c '))
        # systemd expands $NAME itself; every shell variable must be escaped as $$.
        self.assertNotRegex(post[0].replace('$$', ''), r'\$')
        self.assertNotIn('\\', post[0])
        for name in ('SERVICE_RESULT', 'm4-max-power-mw', 'm4-core-mask',
                     'apple,firmware-build', 'dmesg'):
            self.assertIn(name, post[0])


if __name__ == '__main__':
    unittest.main()
