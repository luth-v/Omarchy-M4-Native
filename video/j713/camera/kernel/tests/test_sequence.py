# SPDX-License-Identifier: GPL-2.0-only
"""Exact reviewed production fixture, including wire, pool, IPC and log bytes."""
import hashlib
import json
from pathlib import Path
import subprocess
import unittest

HERE = Path(__file__).resolve().parent


class ProductionSequence(unittest.TestCase):
    def check_trace(self, fixture, option):
        expected = json.loads((HERE / fixture).read_text())
        result = subprocess.run([str(HERE / '.build/sequence_test'), option],
                                check=True, capture_output=True)
        self.assertEqual(len(result.stdout), expected['trace_bytes'])
        self.assertEqual(hashlib.sha256(result.stdout).hexdigest(), expected['trace_sha256'])

    def test_reviewed_production_bytes(self):
        self.check_trace('session-production-baseline.json', '--production-trace')

    def test_reviewed_production_bytes_without_tail(self):
        self.check_trace('session-production-baseline.json', '--production-trace-no-tail')

    def test_reviewed_1080p_bytes(self):
        self.check_trace('session-1080p-baseline.json', '--1080p-trace')


if __name__ == '__main__':
    unittest.main()
